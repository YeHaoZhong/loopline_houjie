# src/ftp_image_uploader/log_setup.py
import logging
import threading
from pathlib import Path
from datetime import datetime

DEFAULT_FORMAT = "%(asctime)s %(levelname)s [%(threadName)s] %(name)s: %(message)s"
DEFAULT_DATEFMT = "%Y-%m-%d %H:%M:%S"

class HourlyFileHandler(logging.Handler):
    """
    自实现的按小时写入文件的 handler。
    每条记录会检查当前时间的 hour 是否与已打开文件相同；
    如果不同则创建新的日志文件（路径：base_dir/YYYY/MM/DD/log_YYYY-MM-DD_HH.txt）。
    """
    def __init__(self, base_dir, encoding="utf-8", level=logging.NOTSET, formatter=None):
        super().__init__(level)
        self.base_dir = Path(base_dir)
        self.encoding = encoding
        self._lock = threading.RLock()
        self._current_hour_id = None  # 格式: "YYYY-MM-DD_HH"
        self._file_handler = None
        self.setFormatter(formatter or logging.Formatter(DEFAULT_FORMAT, DEFAULT_DATEFMT))

    def _hour_id(self, now: datetime):
        return now.strftime("%Y-%m-%d_%H")

    def _make_log_path(self, now: datetime) -> Path:
        """返回完整的日志文件路径并确保目录存在"""
        y = now.strftime("%Y")
        m = now.strftime("%m")
        d = now.strftime("%d")
        hour_id = self._hour_id(now)
        dir_path = self.base_dir / y / m / d
        dir_path.mkdir(parents=True, exist_ok=True)
        filename = f"log_{hour_id}.txt"
        return dir_path / filename

    def _open_file_handler(self, now: datetime):
        """关闭旧 handler，打开并绑定到新的 FileHandler"""
        # 关闭旧的
        if self._file_handler:
            try:
                self._file_handler.close()
            except Exception:
                pass
        # 打开新文件
        log_path = self._make_log_path(now)
        fh = logging.FileHandler(log_path, encoding=self.encoding, mode="a")
        fh.setLevel(self.level)
        fh.setFormatter(self.formatter)
        self._file_handler = fh
        self._current_hour_id = self._hour_id(now)

    def emit(self, record):
        """
        每次 emit 时检查是否需要切换文件（按当前时间的小时）
        并把 record 交给内部的 file handler 处理。
        """
        try:
            now = datetime.now()
            hour_id = self._hour_id(now)
            with self._lock:
                if self._file_handler is None or hour_id != self._current_hour_id:
                    self._open_file_handler(now)
                # 交给内部 handler 写入
                self._file_handler.emit(record)
        except Exception:
            # 如果写日志也出错，使用 logging 的异常处理，避免抛出
            self.handleError(record)

    def close(self):
        with self._lock:
            if self._file_handler:
                try:
                    self._file_handler.close()
                except Exception:
                    pass
                self._file_handler = None
            super().close()


def setup_logging(base_log_dir,
                  level=logging.INFO,
                  console=True,
                  console_level=None,
                  encoding="utf-8"):
    """
    初始化根日志：
      - base_log_dir: 日志根目录（可以是字符串或 Path）
      - level: 根 logger level
      - console: 是否输出到控制台
      - console_level: 控制台输出级别（默认与 level 相同）
    """
    base_log_dir = Path(base_log_dir)
    base_log_dir.mkdir(parents=True, exist_ok=True)

    # 清理已有根 handler（避免重复配置）
    root = logging.getLogger()
    for h in list(root.handlers):
        root.removeHandler(h)
        try:
            h.close()
        except Exception:
            pass

    root.setLevel(level)

    # 文件 handler（每小时切换）
    formatter = logging.Formatter(fmt=DEFAULT_FORMAT, datefmt=DEFAULT_DATEFMT)
    file_handler = HourlyFileHandler(base_log_dir, encoding=encoding, level=level, formatter=formatter)
    root.addHandler(file_handler)

    # 控制台 handler（可选）
    if console:
        ch = logging.StreamHandler()
        ch.setLevel(console_level if console_level is not None else level)
        ch.setFormatter(formatter)
        root.addHandler(ch)

    logging.getLogger(__name__).info("Logging initialized. base_dir=%s, level=%s", str(base_log_dir), logging.getLevelName(level))
    return file_handler  # 返回以便在需要时关闭
