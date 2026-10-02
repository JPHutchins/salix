from salix import Struct


class FileError(OSError, Struct, frozen=True):
    code: int = 0


result = {
    "file_error": FileError(2, "missing"),
    "keyword_code": ("code",),
    "code_change": object(),
}
