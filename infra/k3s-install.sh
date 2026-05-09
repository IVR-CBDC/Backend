#!/bin/bash
set -euo pipefail

echo "=== k3s local environment setup ==="

# 1. Install k3s if not present
if ! command -v k3s &>/dev/null; then
  echo "Installing k3s..."
  curl -sfL https://get.k3s.io | INSTALL_K3S_EXEC="--write-kubeconfig-mode 644" sh -
else
  echo "k3s already installed, skipping"
fi

# 2. Kubeconfig for kubectl/helm
echo "Setting up kubeconfig..."
mkdir -p ~/.kube
sudo cp /etc/rancher/k3s/k3s.yaml ~/.kube/config
sudo chown "$(id -u):$(id -g)" ~/.kube/config
export KUBECONFIG=~/.kube/config

# 3. Wait for k3s to be ready
echo "Waiting for k3s node to be Ready..."
kubectl wait --for=condition=Ready node --all --timeout=120s

# 4. Local registry (localhost:5000)
if ! docker ps --format '{{.Names}}' | grep -q '^registry$'; then
  echo "Starting local registry on localhost:5000..."
  docker run -d -p 5000:5000 --restart=always --name registry registry:2
else
  echo "Local registry already running"
fi

# 5. Configure k3s to use local registry without TLS
echo "Configuring k3s registry mirror..."
sudo mkdir -p /etc/rancher/k3s
sudo tee /etc/rancher/k3s/registries.yaml > /dev/null <<EOF
mirrors:
  "localhost:5000":
    endpoint:
      - "http://localhost:5000"
EOF
sudo systemctl restart k3s

# Wait for k3s to come back after restart
echo "Waiting for k3s to restart..."
sleep 5
sudo chmod 644 /etc/rancher/k3s/k3s.yaml
# Re-copy kubeconfig after restart (k3s may regenerate it)
sudo cp /etc/rancher/k3s/k3s.yaml ~/.kube/config
sudo chown "$(id -u):$(id -g)" ~/.kube/config
kubectl wait --for=condition=Ready node --all --timeout=120s

# Wait for CoreDNS to be ready
echo "Waiting for CoreDNS..."
kubectl wait --for=condition=Ready pod -l k8s-app=kube-dns -n kube-system --timeout=120s

# 6. Create namespaces
echo "Creating namespaces..."
kubectl create namespace backend --dry-run=client -o yaml | kubectl apply -f -
kubectl create namespace data --dry-run=client -o yaml | kubectl apply -f -

# 7. Create JWT keys secret in backend namespace
if [ -d "infra/keys" ]; then
  echo "Creating jwt-keys secret..."
  kubectl create secret generic jwt-keys \
    --from-file=jwt_private.jwk=infra/keys/jwt_private.jwk \
    --from-file=jwt_public.jwk=infra/keys/jwt_public.jwk \
    --from-file=jwt_public.pem=infra/keys/jwt_public.pem \
    -n backend --dry-run=client -o yaml | kubectl apply -f -
else
  echo "WARNING: infra/keys/ not found. Run 'make keys' first, then re-run this script."
fi

echo ""
echo "=== Done ==="
echo "  Namespaces:  backend, data"
echo "  Registry:    localhost:5000"
echo "  Kubeconfig:  ~/.kube/config"
echo ""
echo "Next: make up-k3s"
