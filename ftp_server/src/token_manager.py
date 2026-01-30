# src/ftp_image_uploader/token_manager.py
import threading
import time
import logging
from typing import Optional
import requests
import sys, os
sys.path.append(os.path.dirname(__file__))
import config

log = logging.getLogger("TokenManager")

class TokenManager:
    """
    线程安全的 token 管理器。按需登录并缓存 token。
    用法:
        tm = TokenManager()
        token = tm.get_token()  # 若无 token 则登录
        token = tm.get_token(force=True)  # 强制刷新
    """
    def __init__(self, session: Optional[requests.Session] = None):
        self._lock = threading.RLock()
        self._token = None
        self._last_refresh_ts = 0.0
        # 如果后端返回 expires 字段可用来提前刷新；暂时不依赖后端过期时间
        self._expires_at = None
        self._session = session or requests.Session()

    def _do_login(self) -> Optional[str]:
        """实际向登录接口请求 token，失败返回 None"""
        url = config.LOGIN_URL
        payload = config.LOGIN_PAYLOAD.copy()
        max_retries = getattr(config, "LOGIN_MAX_RETRIES", 3)
        timeout = getattr(config, "LOGIN_TIMEOUT", 10)
        backoff_base = getattr(config, "LOGIN_RETRY_BACKOFF", 2.0)

        for attempt in range(1, max_retries + 1):
            try:
                log.info("[登录] 第 %d 次请求登录接口: %s", attempt, url)
                r = self._session.post(url, json=payload, timeout=timeout)
                text_snip = (r.text[:1000] + "...") if r.text and len(r.text) > 1000 else r.text
                log.debug("[登录返回] status=%s body=%s", r.status_code, text_snip)
                if r.status_code != 200:
                    log.warning("[登录失败] status=%s body=%s", r.status_code, text_snip)
                    raise requests.RequestException(f"login status {r.status_code}")

                try:
                    jr = r.json()
                except Exception:
                    log.exception("[登录解析 JSON 失败] body=%s", text_snip)
                    raise

                # 检查返回结构：你的示例为 {"data":{"token": "..."}}
                data = jr.get("data") if isinstance(jr, dict) else None
                if data and isinstance(data, dict):
                    token = data.get("token")
                    # 若返回中包含过期时间，可以设置 self._expires_at = time.time() + ttl
                    ttl = data.get("expires_in") or data.get("expire") or data.get("expires")
                    if token:
                        log.info("[登录成功] 获得 token（长度=%d）", len(token))
                        return token
                    else:
                        log.warning("[登录返回缺少 token] json=%s", jr)
                else:
                    log.warning("[登录响应 data 无效] json=%s", jr)

            except requests.RequestException as ex:
                log.warning("[登录网络异常] 尝试=%d err=%s", attempt, ex)
            except Exception:
                log.exception("[登录未知异常] 尝试=%d", attempt)

            # backoff
            if attempt < max_retries:
                wait = backoff_base ** attempt
                log.info("[登录] 等待 %.1fs 后重试", wait)
                time.sleep(wait)

        log.error("[登录失败] 超过重试次数(%d)", max_retries)
        return None

    def get_token(self, force: bool = False) -> Optional[str]:
        with self._lock:
            # 如果未强制刷新并且已有 token 并在有效期内（若设置了 expires），则直接返回
            if not force and self._token:
                if self._expires_at is None or time.time() < self._expires_at:
                    return self._token
                else:
                    log.info("[token 已过期] 自动刷新")
            # 否则尝试登录获取新 token
            new_token = self._do_login()
            if new_token:
                self._token = new_token
                self._last_refresh_ts = time.time()
                # 如果后端提供 expires 时间，这里可以设置 self._expires_at
                # self._expires_at = time.time() + ttl_seconds
                return self._token
            else:
                # 登录失败，保持旧 token（如果有），并返回 None 或旧 token 视策略而定。
                # 这里返回旧 token（可能为 None）。调用方应对 None 做处理。
                return self._token

# 单例（便于在项目中直接导入使用）
_default_mgr = None
_def_lock = threading.Lock()

def default_manager():
    global _default_mgr
    with _def_lock:
        if _default_mgr is None:
            _default_mgr = TokenManager()
        return _default_mgr
