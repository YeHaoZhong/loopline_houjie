# src/ftp_image_uploader/db.py
import logging
from mysql.connector.pooling import MySQLConnectionPool
from typing import List, Dict

_pool = None

def init_pool(db_config: dict):
    global _pool
    if _pool is None:
        cfg = db_config.copy()
        pool_name = cfg.pop("pool_name", "ftp_pool")
        pool_size = cfg.pop("pool_size", 5)
        _pool = MySQLConnectionPool(pool_name=pool_name, pool_size=pool_size, **cfg)
        logging.info("DB pool initialized, size=%s", pool_size)

def get_conn():
    if _pool is None:
        raise RuntimeError("DB pool not initialized. Call init_pool first.")
    return _pool.get_connection()

def fetch_pending(limit: int = 50) -> List[Dict]:
    conn = get_conn()
    try:
        cur = conn.cursor(dictionary=True)
        cur.execute("SELECT id, code, path FROM pic WHERE is_upload = 0 LIMIT %s", (limit,))
        rows = cur.fetchall()
        cur.close()
        return rows
    finally:
        conn.close()

def exists_code(code: str) -> bool:
    conn = get_conn()
    try:
        cur = conn.cursor()
        cur.execute("SELECT 1 FROM pic WHERE code = %s LIMIT 1", (code,))
        r = cur.fetchone()
        cur.close()
        return bool(r)
    finally:
        conn.close()

def insert_or_update_pic(code: str, path: str):
    conn = get_conn()
    try:
        cur = conn.cursor()
        cur.execute("SELECT id FROM pic WHERE code = %s LIMIT 1", (code,))
        r = cur.fetchone()
        if r:
            cur.execute("UPDATE pic SET path=%s, time=NOW(), is_upload=0 WHERE code=%s", (path, code))
        else:
            cur.execute("INSERT INTO pic (code, path, time, is_upload) VALUES (%s,%s,NOW(),0)", (code, path))
        conn.commit()
        cur.close()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()

def mark_uploaded(code: str):
    conn = get_conn()
    try:
        cur = conn.cursor()
        cur.execute("UPDATE pic SET is_upload = 1, upload_time = NOW() WHERE code = %s", (code,))
        conn.commit()
        cur.close()
    finally:
        conn.close()

def mark_missing(pic_id: int):
    conn = get_conn()
    try:
        cur = conn.cursor()
        cur.execute("UPDATE pic SET is_upload = 2 WHERE id = %s", (pic_id,))
        conn.commit()
        cur.close()
    finally:
        conn.close()

def delete_terminal_table():
    conn = get_conn()
    try:
        cur = conn.cursor()
        cur.execute("DELETE FROM terminalcode_to_slot;")
        conn.commit()
        cur.close()
    finally:
        conn.close()

def update_short_url_by_code(code: str, short_url: str):
    """
    根据 code 更新 short_url 字段（如果需要，也可以在 insert_or_update_pic 里处理）
    """
    conn = get_conn()
    try:
        cur = conn.cursor()
        cur.execute("UPDATE pic SET short_url = %s WHERE code = %s", (short_url, code))
        conn.commit()
        cur.close()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()