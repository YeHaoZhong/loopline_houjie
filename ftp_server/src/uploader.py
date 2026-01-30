# src/ftp_image_uploader/uploader.py
import threading
import time
import logging
import requests
import sys, os
sys.path.append(os.path.dirname(__file__))
import db, config
from utils import is_token_invalid_response
from token_manager import default_manager

class Uploader:
    def __init__(self,token_manager=None):
        self._stop_event = threading.Event()
        self._thread = None
        self._session = requests.Session()
        self.token_manager = token_manager or default_manager()

    def start(self):
        if self._thread and self._thread.is_alive():
            return
        self._thread = threading.Thread(target=self._run, daemon=True, name="图片上传线程")
        self._thread.start()
        logging.info("启动图片上传线程")

    def stop(self, timeout=5):
        self._stop_event.set()
        if self._thread:
            self._thread.join(timeout)

    def _run(self):
        import os
        import time
        import logging
        import requests
        from concurrent.futures import ThreadPoolExecutor, as_completed

        log = logging.getLogger("图片上传线程")
        attempts = {}  # key = code -> attempt count

        while not self._stop_event.is_set():
            try:
                # 拉取待处理行（最多 50 条）
                rows = db.fetch_pending(limit=50)
                if not rows:
                    time.sleep(config.POLL_INTERVAL_SECONDS)
                    continue

                # 建立 code -> row 映射（方便后续根据 waybillNo 找到对应记录）
                rows_by_code = {}
                payload_items = []
                for row in rows:
                    pic_id = row.get("id")
                    code = row.get("code")
                    path = row.get("path")
                    # 过滤：文件不存在 -> 直接标记缺失
                    if not path or not os.path.exists(path):
                        log.error("单号: %s 图片不存在, 标记为缺失: %s", code, path)
                        try:
                            db.mark_missing(pic_id)
                        except Exception:
                            log.exception("标记缺失失败: %s", pic_id)
                        continue

                    # 过滤：超过重试上限
                    attempts.setdefault(code, 0)
                    if attempts[code] >= config.MAX_RETRIES_PER_FILE:
                        log.warning("请求达到重试上限, 跳过: %s", code)
                        continue

                    # 构建 POST 的单项元数据（fileName、size、scanType、waybillNo）
                    try:
                        size = os.path.getsize(path)
                    except Exception:
                        size = 0
                    item = {
                        "fileName": os.path.basename(path),
                        "size": str(size),
                        "scanType": str("107"),                                 #扫描类型,入仓:106,集货到件 :101,集货到件带收入发:103,卸车到件 :107,卸车到件带收入发:102,出仓:104,装车发件 :105
                        "waybillNo": code
                    }
                    payload_items.append(item)
                    rows_by_code[code] = row

                # 若没有有效要请求的 item，继续下一轮
                if not payload_items:
                    time.sleep(0.5)
                    continue

                # 获取 token（可能为 None）
                token = self.token_manager.get_token()
                if not token:
                    log.warning("[Token 未获取到] 本轮跳过，稍后重试")
                    time.sleep(1)
                    continue

                headers = {config.AUTH_HEADER_NAME: token, "Content-Type": "application/json"}

                # 发起一次批量 POST（json array）
                try:
                    post_resp = self._session.post(config.UPLOAD_ENDPOINT,
                                                   json=payload_items,
                                                   headers=headers,
                                                   timeout=config.UPLOAD_TIMEOUT)
                except requests.RequestException as e:
                    log.warning("[POST 网络异常] err=%s", e)
                    # 增加 attempt 对所有这次提交的 codes（防止永远不变）
                    for it in payload_items:
                        c = it.get("waybillNo")
                        attempts[c] = attempts.get(c, 0) + 1
                    time.sleep(1)
                    continue

                # 记录返回（方便排查）
                body_preview = post_resp.text if post_resp.text else ""
                log.info("[POST 返回] status=%s body=%s", post_resp.status_code, body_preview)

                # 检测 token 失效（HTTP 401/403 或业务 code）
                jr = None
                try:
                    jr = post_resp.json()
                except Exception:
                    jr = None
                
                if post_resp.ok and jr and isinstance(jr.get("data"), list) and len(jr["data"]) > 0:            #写入shortUrl
                    items = jr["data"]
                    for it in items:
                        waybill_no = it.get("waybillNo")
                        short_url = it.get("shortUrl")  # 注意 key 名称大小写按返回体
                        if not waybill_no:
                            # 尝试从 fileName 解析（和你的 do_put 一致）
                            fn = it.get("fileName")
                            if fn:
                                waybill_no = fn.split("_")[0] if "_" in fn else fn
                        if waybill_no and short_url:
                            try:
                                db.update_short_url_by_code(waybill_no, short_url)
                            except Exception:
                                log.exception("[DB] 更新 shortUrl 失败: %s -> %s", waybill_no, short_url)
                
                token_invalid = False
                if is_token_invalid_response(post_resp, jr):
                    token_invalid = True

                # 若 token 无效则尝试强制刷新并重试一次 POST（仅重试一次）
                if token_invalid:
                    log.info("[POST] 检测到 token 失效，尝试刷新并重试一次 POST")
                    new_token = self.token_manager.get_token(force=True)
                    if not new_token:
                        log.error("[POST 重试] 刷新 token 失败，标记所有此次批次 codes 为重试并跳过")
                        for it in payload_items:
                            c = it.get("waybillNo")
                            attempts[c] = attempts.get(c, 0) + 1
                        time.sleep(1)
                        continue
                    headers[config.AUTH_HEADER_NAME] = new_token
                    try:
                        post_resp = self._session.post(config.UPLOAD_ENDPOINT,
                                                       json=payload_items,
                                                       headers=headers,
                                                       timeout=config.UPLOAD_TIMEOUT)
                    except requests.RequestException as e:
                        log.warning("[POST 重试 网络异常] err=%s", e)
                        for it in payload_items:
                            c = it.get("waybillNo")
                            attempts[c] = attempts.get(c, 0) + 1
                        time.sleep(1)
                        continue

                    body_preview = post_resp.text[:2000] if post_resp.text else ""
                    log.info("[POST 重试返回] status=%s body=%s", post_resp.status_code, body_preview)
                    try:
                        jr = post_resp.json()
                    except Exception:
                        jr = None

                # 若 POST 成功并返回 data 列表，进行 PUT 上传
                if post_resp.ok and jr and isinstance(jr.get("data"), list) and len(jr["data"]) > 0:
                    items = jr["data"]
                    # 使用线程池并发 PUT（最大并发受 config.MAX_UPLOAD_CONCURRENT 限制）
                    max_workers = min(config.MAX_UPLOAD_CONCURRENT or 3, len(items))
                    if max_workers <= 0:
                        max_workers = 1

                    def do_put(item):
                        """
                        单个 PUT 的工作函数。返回 (code, success_bool, errmsg_optional)
                        """
                        upload_url = item.get("uploadUrl")
                        content_type = item.get("contentType") or "application/octet-stream"
                        waybill_no = item.get("waybillNo")
                        # 如果返回没有 waybillNo, 尝试从 fileName 解析
                        if not waybill_no:
                            waybill_no = None
                            fn = item.get("fileName")
                            if fn:
                                # 试用 filename 的前缀作为 waybillNo（如 JT...）
                                waybill_no = fn.split("_")[0] if "_" in fn else fn

                        if not upload_url:
                            return (waybill_no, False, "missing uploadUrl")

                        row = rows_by_code.get(waybill_no)
                        if not row:
                            return (waybill_no, False, "no matching row for waybillNo")

                        pic_id = row.get("id")
                        path = row.get("path")
                        if not path or not os.path.exists(path):
                            # 标记缺失
                            try:
                                db.mark_missing(pic_id)
                            except Exception:
                                log.info("标记缺失失败: %s", pic_id)
                            return (waybill_no, False, "file missing")

                        try:
                            with open(path, "rb") as pf:
                                put_headers = {"Content-Type": content_type}
                                put_resp = self._session.put(upload_url, data=pf, headers=put_headers, timeout=config.UPLOAD_TIMEOUT)

                            if put_resp.status_code in (200, 201, 204):
                                try:
                                    db.mark_uploaded(code=waybill_no)
                                except Exception:
                                    log.info("DB 标记失败: code=%s", waybill_no)
                                return (waybill_no, True, None)
                            else:
                                return (waybill_no, False, f"put_status={put_resp.status_code} text={put_resp.text[:200]}")
                        except requests.RequestException as e:
                            return (waybill_no, False, f"put_exception:{e}")
                        except Exception as e:
                            return (waybill_no, False, f"put_unknown:{e}")

                    # 并发执行所有 PUT
                    futures = []
                    with ThreadPoolExecutor(max_workers=max_workers) as exc:
                        for it in items:
                            futures.append(exc.submit(do_put, it))

                        for fut in as_completed(futures):
                            try:
                                waybill_no, success, errmsg = fut.result()
                            except Exception as e:
                                log.info("PUT 任务未预期异常: %s", e)
                                continue

                            if success:
                                attempts.pop(waybill_no, None)
                                log.info("[PUT 上传成功] waybillNo=%s", waybill_no)
                            else:
                                # 失败增加尝试次数（若 waybill_no 为 None，则跳过 or 批量处理）
                                if waybill_no:
                                    attempts[waybill_no] = attempts.get(waybill_no, 0) + 1
                                    log.info("[PUT 失败] waybillNo=%s err=%s attempts=%d", waybill_no, errmsg, attempts[waybill_no])
                                else:
                                    log.info("[PUT 失败 无法确定 waybillNo] err=%s", errmsg)

                else:
                    # POST 本身失败或未返回 data -> 对本次所有 codes 增加重试计数
                    for it in payload_items:
                        c = it.get("waybillNo")
                        attempts[c] = attempts.get(c, 0) + 1
                    log.warning("[POST 失败或未返回 data] status=%s body=%s", post_resp.status_code, (post_resp.text[:1000] if post_resp.text else ""))
            except Exception:
                log.exception("Uploader 主循环异常，稍后重试")
                time.sleep(config.POLL_INTERVAL_SECONDS)

            # 小睡，防止热循环
            time.sleep(0.2)

