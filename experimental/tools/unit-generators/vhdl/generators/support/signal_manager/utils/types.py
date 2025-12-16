from typing import TypedDict

try:  # Python 3.11+
    from typing import NotRequired  # type: ignore[attr-defined]
except ImportError:  # Python <3.11
    try:
        from typing_extensions import NotRequired  # type: ignore[misc]
    except ModuleNotFoundError:
        class _NotRequired:
            def __class_getitem__(cls, item):
                return item

        NotRequired = _NotRequired  # type: ignore[misc,assignment]
from generators.support.utils import ExtraSignals


class Channel(TypedDict):
    name: str
    bitwidth: int
    extra_signals: NotRequired[ExtraSignals]
    size: NotRequired[int]
