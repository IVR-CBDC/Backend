{
  "app": {
    "threads_num": 4,
    "log": { "log_level": "INFO" }
  },
  "listeners": [
    { "address": "0.0.0.0", "port": ${SERVICE_PORT}, "https": false }
  ],
  "db_clients": [
    {
      "name": "default",
      "rdbms": "postgresql",
      "host": "${DB_HOST}",
      "port": ${DB_PORT},
      "dbname": "${DB_NAME}",
      "user": "${DB_USER}",
      "passwd": "${DB_PASSWORD}",
      "is_fast": false,
      "connection_number": 8
    }
  ]
}
