#!/usr/bin/env python3
import os
import re
import shutil
import logging
from datetime import datetime
from pyftpdlib.servers import FTPServer
from pyftpdlib.handlers import FTPHandler, TLS_FTPHandler
from pyftpdlib.authorizers import DummyAuthorizer
from PIL import Image  # 用于简单验证是图片
import mysql.connector
import threading
import time
import requests

# ========== 配置区 ==========
FTP_USER = "user"
FTP_PASS = "666666"
FTP_HOME = r"E:\FTP\test"   # 本地存储根目录（必须存在或代码会创建）
LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 21

# 被动端口范围（如果在路由器/NAT后面，需要映射这些端口）
PASV_PORTS = range(60000, 60020)  # 示例：10 个被动端口（按需扩大）

# MySQL 配置
db = mysql.connector.connect(
    host = "localhost",
    user = "root",
    password = "123456",
    database = "loopline_byplc"
)

# 是否启用 TLS（FTPS, 推荐生产开启）
USE_TLS = False
TLS_CERT = "/path/to/cert.pem"
TLS_KEY  = "/path/to/key.pem"

# 最大允许单文件大小 (bytes)，可选
MAX_FILE_SIZE = 50 * 1024 * 1024 

# 图片上传请求
UPLOAD_ENDPOINT = "http://example.com/api/upload"  # <-- 改为你实际的上传接口
POLL_INTERVAL_SECONDS = 10      # 后台线程查询间隔
UPLOAD_TIMEOUT = 30             # requests 超时时间（秒）
MAX_UPLOAD_CONCURRENT = 3       # 本线程每个循环同时上传多少个（简单并发限制）
MAX_RETRIES_PER_FILE = 3       # 单个运行期内的本地重试次数

# ========= 日志 ==========
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s: %(message)s")

def get_db_connection():
    return mysql.connector.connect(
        host="localhost",
        user="root",
        password="123456",
        database="loopline_byplc",
        autocommit=False  # 我们显式 commit/rollback
    )

# ========= 工具函数 ==========
def ensure_dir(path):
    os.makedirs(path, exist_ok=True)

def is_image_file(path):
    """用 PIL 尝试打开以判断是否为有效图片文件（基础验证）"""
    try:
        with Image.open(path) as im:
            im.verify()
        return True
    except Exception:
        return False

def parse_filename_for_id_date_hour(filename):
    """
    更稳健的解析：
      - id: 文件名开头连续的数字 (如 "5_..." -> "5")
      - date: 优先匹配 YYYY[-_]?MM[-_]?DD (如 2025-09-20 或 20250920)
      - hour: 在 date 之后查找连续数字串，取前两位作为小时（00-23）。若无则取当前小时。
    返回 (id_str, year, month, day, hour_str)
    """
    base = filename  # 已经是 basename 的话就直接传入
    now = datetime.now()

    # 1) id: 文件名前面开头连续的数字
    m_id = re.match(r'^\s*(\d+)', base)
    id_str = m_id.group(1) if m_id else "unknown"

    # 2) 优先匹配以 19xx 或 20xx 开头的日期格式，允许 - 或 _ 或 无分隔
    #    例如: 20250920, 2025-09-20, 2025_09_20
    m_date = re.search(r'(19|20)\d{2}(?:[-_]?)(0[1-9]|1[0-2])(?:[-_]?)([0-3][0-9])', base)
    if m_date:
        year = m_date.group(0)[0:4]  # 或 m_date.group(1)+rest，但直接切更稳
        # 更明确提取：
        # year = m_date.group(0)[0:4]
        # month = m_date.group(2)
        # day = m_date.group(3)
        # 但为了兼容不同分隔位置我们也可以做如下：
        year = m_date.group(0)[0:4]
        # 找到 month/day 的位置（使用分组更稳）
        # 使用 groups:
        # groups: (prefix_year_two, month, day)  -> but prefix_year_two is '19' or '20'
        # To get full year: take first 4 chars of matched substring
        matched = m_date.group(0)
        # 规范化 month/day 从 matched 中提取最后 4 或 2 两部分：
        # simpler: use the capture groups for month/day:
        month = m_date.group(2)
        day = m_date.group(3)

        # 3) 查找时间串（从 date 匹配结束位置之后）
        after_pos = m_date.end()
        m_time_after = re.search(r'([0-9]{2,})', base[after_pos:])  # 至少两位数字作为时间候选
        if m_time_after:
            time_str = m_time_after.group(1)
            hour = time_str[:2]
        else:
            hour = now.strftime("%H")
    else:
        # 若没找到明确的 YYYYMMDD 格式，再尝试宽松匹配 8 连续数字但要求以 20 或 19 开头
        m_date2 = re.search(r'(20|19)([0-9]{6})', base)
        if m_date2:
            matched = m_date2.group(0)   # 比如 20250920
            year = matched[0:4]
            month = matched[4:6]
            day = matched[6:8]
            after_pos = m_date2.end()
            m_time_after = re.search(r'([0-9]{2,})', base[after_pos:])
            if m_time_after:
                hour = m_time_after.group(1)[:2]
            else:
                hour = now.strftime("%H")
        else:
            # 完全找不到日期 -> 回退到当前日期/时间
            year = now.strftime("%Y")
            month = now.strftime("%m")
            day = now.strftime("%d")
            hour = now.strftime("%H")

    # 规范化 hour 两位
    hour = hour.zfill(2)[:2]

    return id_str, year, month, day, hour
def ensure_insert(scan_code):
    try:
        sql = "SELECT path FROM pic WHERE code = %s"
        values = (scan_code,)
        cursor.execute(sql,values)
        results = cursor.fetchall()
        if len(results) == 0:
            logging.info("[图片查询] 单号:%s, 结果:%s",scan_code,results)
            return True
        else:
            print("[图片查询] 单号:%s, 结果:%s",scan_code,results)
            return False
    except Exception as e:
        logging.exception("[查询异常] 单号:%s, 异常:%s",scan_code,e)
        return True
# ========= FTP Handler ==========---
class MyFTPHandler(FTPHandler):
    def on_file_received(self, file_path):
        """
        当文件上传完成后被调用（注意：上传完成之后文件已经在 FTP_HOME 的某处）
        file_path 是服务器上临时文件路径（相对或绝对，取决于配置）
        """
        logging.info("Received file: %s", file_path)
        
        # 1) 大小检查
        try:
            size = os.path.getsize(file_path)
            if size > MAX_FILE_SIZE:
                logging.warning("文件过大 (%d bytes). 移除: %s", size, file_path)
                os.remove(file_path)
                return
        except Exception as e:
            logging.exception("无法检查该文件: %s", e)
            return

        # 2) 图片验证
        if not is_image_file(file_path):
            logging.warning("上传的不是图片, 移除: %s", file_path)
            os.remove(file_path)
            return

        # 3) 解析文件名
        filename = os.path.basename(file_path)
        supply_id, year, month, day, hour = parse_filename_for_id_date_hour(filename)
        if supply_id != "unknown":
            dest_dir = os.path.join(FTP_HOME, supply_id+"号供包台", year, month, day, hour)
        else:
            dest_dir = os.path.join(FTP_HOME, "unknown", year, month, day, hour)
        ensure_dir(dest_dir)

        # 5) 防止重名：使用 uuid 或在文件名后加时间戳
        split_name = re.split('_',filename)
        scan_code = split_name[1]
        save_name = scan_code+'_'+split_name[2]
        
        dest_path = os.path.join(dest_dir, save_name)

        try:
            shutil.move(file_path, dest_path)  # 将文件移动到目标目录
        except Exception as e:
            logging.exception("[图片保存异常] 单号:%s, 异常信息:%s",scan_code, e)
            # 若移动失败，保留原文件以便人工检查
            dest_path = file_path

        try:
            ok = ensure_insert(scan_code)
            if ok:  #还未写入, 插入
                sql = "INSERT INTO pic (code,path,time,is_upload) VALUES (%s,%s, NOW(),%s)"
                values = (scan_code,dest_path,0)
                cursor.execute(sql,values)
                db.commit()
                logging.info("[图片插入] 单号:%s",scan_code)
            else:
                sql = "UPDATE pic SET path = %s, time = NOW(), is_upload = %s WHERE code = %s"
                values = (dest_path,0,scan_code)
                cursor.execute(sql,values)
                db.commit()
                logging.info("[图片更新] 单号:%s",scan_code)
        except Exception as e:
            logging.exception("[写入异常]: %s", e)
            # 如果写 DB 失败，可以考虑把文件移动到一个 error 目录，或用消息队列重试
            err_dir = os.path.join(FTP_HOME, "db_error")
            ensure_dir(err_dir)
            shutil.copy(dest_path, os.path.join(err_dir, os.path.basename(dest_path)))

# ========= 启动服务器 ==========
def start_ftp_server():
    ensure_dir(FTP_HOME)

    authorizer = DummyAuthorizer()
    # 添加一个普通用户，home dir 为 FTP_HOME，权限 "elradfmw" 意味：读取/写入/删除等
    authorizer.add_user(FTP_USER, FTP_PASS, FTP_HOME, perm="elradfmw")

    # 选择 handler（支持 TLS 的可用 TLS_FTPHandler）
    handler_cls = TLS_FTPHandler if USE_TLS else MyFTPHandler
    handler = handler_cls
    handler.authorizer = authorizer

    # 被动端口配置
    handler.passive_ports = PASV_PORTS

    # TLS 配置（如果启用）
    if USE_TLS:
        handler.certfile = TLS_CERT
        handler.keyfile = TLS_KEY

    address = (LISTEN_HOST, LISTEN_PORT)
    server = FTPServer(address, handler)
    # 可设置并发限制等
    server.max_cons = 50
    server.max_cons_per_ip = 20

    logging.info("开启图片上传服务")
    server.serve_forever()

# 上传请求
def uploader_worker(stop_event):
    """
    后台循环：查找 is_upload = 0 的记录，并尝试上传文件。
    stop_event: threading.Event()，用于优雅停止（如果需要）。
    """
    # 进程内重试计数器，key=code -> attempts
    attempts = {}

    while not stop_event.is_set():
        try:
            conn = get_db_connection()
            cursor_local = conn.cursor(dictionary=True)  # 方便用列名访问
            # 一次查询若干条待上传记录（避免一次抓取过多）
            cursor_local.execute("SELECT id, code, path FROM pic WHERE is_upload = 0 LIMIT 50")
            rows = cursor_local.fetchall()
            if not rows:
                cursor_local.close()
                conn.close()
                # 没有待处理条目，睡眠一段时间再查
                time.sleep(POLL_INTERVAL_SECONDS)
                continue

            # 逐条处理（可改为线程池并发，这里做简单控制）
            for row in rows:
                if stop_event.is_set():
                    break
                pic_id = row.get("id")
                code = row.get("code")
                path = row.get("path")

                # 防止瞬时 db 中已有但磁盘上没有的情况
                if not path or not os.path.exists(path):
                    logging.error("[上传失败] 文件不存在, code=%s, path=%s", code, path)
                    try:
                        # 标记为 2=文件不存在（便于人工处理）
                        update_sql = "UPDATE pic SET is_upload = %s WHERE id = %s"
                        cursor_local.execute(update_sql, (2, pic_id))
                        conn.commit()
                    except Exception:
                        logging.exception("标记文件不存在时 DB 更新失败: code=%s", code)
                        conn.rollback()
                    continue

                # 限制每个文件的重试次数（仅在当前进程有效）
                attempts.setdefault(code, 0)
                if attempts[code] >= MAX_RETRIES_PER_FILE:
                    logging.warning("[多次失败] 超过重试次数, 跳过: code=%s", code)
                    # 不改 is_upload，让下次或人工处理（也可设置为某个失败状态）
                    continue

                # 尝试上传
                try:
                    with open(path, "rb") as f:
                        files = {"file": (os.path.basename(path), f, "application/octet-stream")}
                        # 如果你的接口需要额外字段（例如 code），放到 data 中
                        data = {"code": code}
                        resp = requests.post(UPLOAD_ENDPOINT, files=files, data=data, timeout=UPLOAD_TIMEOUT)
                    if 200 <= resp.status_code < 300:
                        # 上传成功，更新数据库（设置 is_upload = 1，并记录上传时间）
                        try:
                            update_sql = "UPDATE pic SET is_upload = %s, upload_time = NOW() WHERE id = %s"
                            cursor_local.execute(update_sql, (1, pic_id))
                            conn.commit()
                            logging.info("[上传成功] code=%s, path=%s, resp=%s", code, path, resp.status_code)
                            # 清除 attempts 记录
                            attempts.pop(code, None)
                        except Exception:
                            logging.exception("[DB 更新失败] 上传成功后更新状态失败: code=%s", code)
                            conn.rollback()
                    else:
                        attempts[code] += 1
                        logging.warning("[上传失败] code=%s, status=%s, attempts=%d, resp_text=%s",
                                        code, resp.status_code, attempts[code], resp.text[:200])
                        # 小休息再去下一个文件，失败不改变 is_upload 以便重试
                        time.sleep(1)
                except requests.RequestException as e:
                    attempts[code] += 1
                    logging.warning("[上传异常] code=%s, err=%s, attempts=%d", code, e, attempts[code])
                    time.sleep(1)
                except Exception as e:
                    logging.exception("[未知错误] 上传时发生异常 code=%s: %s", code, e)
                    time.sleep(1)

            cursor_local.close()
            conn.close()
        except Exception as e:
            logging.exception("上传线程主循环异常: %s", e)
            # 如果 DB 或网络短暂问题，等会重试
            time.sleep(POLL_INTERVAL_SECONDS)
def start_uploader_thread():
    stop_event = threading.Event()
    t = threading.Thread(target=uploader_worker, args=(stop_event,), daemon=True, name="UploaderThread")
    t.start()
    return stop_event, t
if __name__ == "__main__":
    logging.info("保存图片的根路径 = %s",FTP_HOME)
    cursor = db.cursor()

    stop_event, uploader_thread = start_uploader_thread()
    try:
        start_ftp_server()
    except KeyboardInterrupt:
        logging.info("收到中断，准备退出...")
    finally:
        # # 优雅停止 uploader（虽然是 daemon 线程，但可通知）
        # stop_event.set()
        # # 若需要等待线程结束可以 join（可选，daemon 模式下不是必须）
        # uploader_thread.join(timeout=5)
        try:
            db.close()
        except Exception:
            pass
