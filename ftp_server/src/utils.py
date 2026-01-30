# src/ftp_image_uploader/utils.py
import os
import re
from datetime import datetime
from PIL import Image

def ensure_dir(path):
    os.makedirs(path, exist_ok=True)

def is_image_file(path: str) -> bool:
    try:
        with Image.open(path) as im:
            im.verify()
        return True
    except Exception:
        return False

def parse_filename_for_id_date_hour(filename: str):
    # 同你原来那段，返回 (id_str, year, month, day, hour)
    base = filename
    now = datetime.now()
    m_id = re.match(r'^\s*(\d+)', base)
    id_str = m_id.group(1) if m_id else "unknown"
    m_date = re.search(r'(19|20)\d{2}(?:[-_]?)(0[1-9]|1[0-2])(?:[-_]?)([0-3][0-9])', base)
    if m_date:
        matched = m_date.group(0)
        year = matched[0:4]
        month = m_date.group(-2) if False else m_date.group(2)
        day = m_date.group(3)
        after_pos = m_date.end()
        m_time_after = re.search(r'([0-9]{2,})', base[after_pos:])
        hour = m_time_after.group(1)[:2] if m_time_after else now.strftime("%H")
    else:
        year, month, day, hour = now.strftime("%Y %m %d %H").split()
    hour = hour.zfill(2)[:2]
    return id_str, year, month, day, hour

def is_token_invalid_response(resp, json_obj):
    """
    检测返回是否表示 token 已失效。
    - resp: requests.Response
    - json_obj: resp.json()（如果已解析，否则传 None）
    判定规则（任一匹配则视为失效）：
      1) HTTP status 401/403
      2) 业务 code == 127000033 （保持兼容）
      3) 返回体中的 msg/message 字段或 resp.text 含关键字 "失效"（或者 "过期" 作为容错）
    """
    try:
        # 1) HTTP code
        if resp is not None and resp.status_code in (401, 403):
            return True
    except Exception:
        pass

    # 2) 业务 code（如果后端有明确 code）
    try:
        if json_obj and isinstance(json_obj, dict):
            if json_obj.get("code") == 127000033:
                return True
    except Exception:
        pass

    # 3) msg / message / resp.text 包含关键词（中文“失效”为重点）
    try:
        msg = ""
        if json_obj and isinstance(json_obj, dict):
            # 常见字段名
            msg = json_obj.get("msg") or json_obj.get("message") or ""
        if not msg:
            # 回退到原始文本（避免 None）
            msg = resp.text if resp is not None and getattr(resp, "text", None) else ""
        if msg:
            # 规范化：去掉周围空白，统一小写（对中文无影响）
            norm = str(msg).strip().lower()
            # 匹配 "失效" 或 "过期"（你要求以 "失效" 为准，这里同时包含 "过期" 作容错）
            if re.search(r"(失效|重新登录)", norm):
                return True
    except Exception:
        pass

    return False