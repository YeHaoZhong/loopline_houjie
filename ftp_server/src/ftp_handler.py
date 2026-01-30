# src/ftp_image_uploader/ftp_handler.py
import logging
import shutil
import os
from pyftpdlib.handlers import FTPHandler
from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.servers import FTPServer
import sys, os
sys.path.append(os.path.dirname(__file__))
import config, utils, db

class MyFTPHandler(FTPHandler):
    def on_file_received(self, file_path):
        logging.info("接收图片路径: %s", file_path)
        try:
            size = os.path.getsize(file_path)
            if size > config.MAX_FILE_SIZE:
                logging.warning("文件过大, 删除: %s", file_path)
                os.remove(file_path)
                return
        except Exception:
            logging.exception("获取文件大小失败")
            return

        if not utils.is_image_file(file_path):
            logging.warning("非图片文件, 删除: %s", file_path)
            os.remove(file_path)
            return

        filename = os.path.basename(file_path)
        supply_id, year, month, day, hour = utils.parse_filename_for_id_date_hour(filename)
        if supply_id != "unknown":
            dest_dir = os.path.join(config.FTP_HOME, f"{supply_id}号供包台", year, month, day, hour)
        else:
            dest_dir = os.path.join(config.FTP_HOME, "unknown", year, month, day, hour)
        utils.ensure_dir(dest_dir)

        # 生成保存名：你可以按照自己的规则改
        parts = filename.split('_')
        if len(parts) >= 3:
            scan_code = parts[1]
            save_name = scan_code + '_' + '_'.join(parts[2:])
        else:
            scan_code = filename
            save_name = filename

        dest_path = os.path.join(dest_dir, save_name)
        try:
            shutil.move(file_path, dest_path)
        except Exception:
            logging.exception("移动文件失败, 保留原路径: %s", file_path)
            dest_path = file_path

        # 写入或更新 DB（注意 db.init_pool 已在 main 中调用）
        try:
            db.insert_or_update_pic(scan_code, dest_path)
            logging.info("保存记录到DB code=%s path=%s", scan_code, dest_path)
        except Exception:
            logging.exception("写 DB 失败, 将文件复制到错误目录")
            err_dir = os.path.join(config.FTP_HOME, "db_error")
            utils.ensure_dir(err_dir)
            shutil.copy(dest_path, os.path.join(err_dir, os.path.basename(dest_path)))

def start_ftp_server():
    utils.ensure_dir(config.FTP_HOME)
    authorizer = DummyAuthorizer()
    authorizer.add_user(config.FTP_USER, config.FTP_PASS, config.FTP_HOME, perm="elradfmw")

    handler = MyFTPHandler
    handler.authorizer = authorizer
    handler.passive_ports = range(config.PASV_PORT_RANGE[0], config.PASV_PORT_RANGE[1]+1)
    address = (config.LISTEN_HOST, config.LISTEN_PORT)
    server = FTPServer(address, handler)
    server.max_cons = 50
    server.max_cons_per_ip = 20
    logging.info("FTP 服务器监听 %s:%s", *address)
    server.serve_forever()
