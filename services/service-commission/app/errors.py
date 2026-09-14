from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse

from app.domain import DomainError

_DOMAIN_STATUS = {"CORRIDOR_NOT_FOUND": 404}


class ApiError(Exception):
    def __init__(self, status: int, code: str, message: str) -> None:
        super().__init__(message)
        self.status = status
        self.code = code
        self.message = message


def _error(status: int, code: str, message: str, **extra) -> JSONResponse:
    return JSONResponse(status_code=status, content={"code": code, "error": message, **extra})


def install_error_handlers(app: FastAPI) -> None:
    @app.exception_handler(ApiError)
    async def _api_error(_: Request, exc: ApiError) -> JSONResponse:
        return _error(exc.status, exc.code, exc.message)

    @app.exception_handler(DomainError)
    async def _domain_error(_: Request, exc: DomainError) -> JSONResponse:
        return _error(_DOMAIN_STATUS.get(exc.code, 400), exc.code, exc.message)

    @app.exception_handler(RequestValidationError)
    async def _validation_error(_: Request, exc: RequestValidationError) -> JSONResponse:
        details = [{"loc": list(e["loc"]), "msg": e["msg"]} for e in exc.errors()]
        return _error(422, "VALIDATION_ERROR", "Некорректные параметры запроса", details=details)
