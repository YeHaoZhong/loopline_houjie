# src/ftp_image_uploader/config.py
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parents[2]

# FTP
FTP_USER = "user"
FTP_PASS = "666666"
FTP_HOME = r"F:\FTP\arrival"
LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 2121  # 如果在 Windows 上, 用非 21 的端口会避免权限问题

PASV_PORT_RANGE = (60000, 60020)

# MySQL
DB_CONFIG = {
    "host": "localhost",
    "port": 3386,
    "user": "root",
    "password": "jdzn123456!@",
    "database": "loopline_houjie",
    "pool_name": "ftp_pool",
    "pool_size": 8,
}

# 登录相关
LOGIN_URL = "https://opa.jtexpress.com.cn/opa/smartLogin"
LOGIN_PAYLOAD = {
    "account": "WD01197700",                                    #进港
    "password": "0f3b4ec9a496cc5be92eccea05899993",
    "appKey": "GZJD001231121",
    "appSecret": "kI8gLrUxTSVaRx0ZjhCwkQ=="
}
UPLOAD_TIMEOUT = 30
# 登录重试/超时等
LOGIN_TIMEOUT = 10
LOGIN_MAX_RETRIES = 3
LOGIN_RETRY_BACKOFF = 2.0  # 指数回退基数（s）

#上传相关
UPLOAD_ENDPOINT = "https://opa.jtexpress.com.cn/opa/smart/scan/getUploadUrl"
AUTH_HEADER_NAME = "authToken"
POLL_INTERVAL_SECONDS = 2
MAX_RETRIES_PER_FILE = 3    #最大重试次数
MAX_UPLOAD_CONCURRENT = 5  #最大并发数

# 文件限制
MAX_FILE_SIZE = 50 * 1024 * 1024

LOGGING_LEVEL = "INFO"
