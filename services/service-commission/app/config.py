import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Settings:
    pg_dsn: str
    jwt_public_key_path: str

    @classmethod
    def from_env(cls) -> "Settings":
        return cls(pg_dsn=os.environ["PG_DSN"], jwt_public_key_path=os.environ["JWT_PUBLIC_KEY_PATH"])
