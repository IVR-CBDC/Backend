import logging

from fastapi import FastAPI, Request
from fastapi.exceptions import RequestValidationError
from fastapi.responses import JSONResponse
from sqlalchemy.exc import DBAPIError
from starlette.exceptions import HTTPException as StarletteHTTPException

from app.domain import DomainError

logger = logging.getLogger(__name__)

_DOMAIN_STATUS = {"CORRIDOR_NOT_FOUND": 404}

_HTTP_STATUS_MESSAGES = {
    404: "Ресурс не найден",
    405: "Метод не поддерживается",
}


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

    @app.exception_handler(StarletteHTTPException)
    async def _http_error(_: Request, exc: StarletteHTTPException) -> JSONResponse:
        message = _HTTP_STATUS_MESSAGES.get(exc.status_code, str(exc.detail))
        code = {404: "NOT_FOUND", 405: "METHOD_NOT_ALLOWED"}.get(exc.status_code, f"HTTP_{exc.status_code}")
        return _error(exc.status_code, code, message)

    @app.exception_handler(DBAPIError)
    @app.exception_handler(OSError)
    @app.exception_handler(TimeoutError)
    async def _db_unavailable(_: Request, exc: Exception) -> JSONResponse:
        logger.warning("database unavailable", exc_info=True)
        return _error(503, "DB_UNAVAILABLE", "База данных недоступна")

    @app.exception_handler(Exception)
    async def _internal_error(_: Request, exc: Exception) -> JSONResponse:
        logger.exception("unhandled error")
        return _error(500, "INTERNAL_ERROR", "Внутренняя ошибка сервиса")
