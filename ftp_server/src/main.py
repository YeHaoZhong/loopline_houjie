# src/ftp_image_uploader/main.py
import sys, os
sys.path.append(os.path.dirname(__file__))
import logging
from pathlib import Path
import config, db, ftp_handler, uploader, read_excel
import signal
import sys
from log_setup import setup_logging
import threading

def main():

    log_root = Path(config.BASE_DIR)/"logs" if hasattr(config, "BASE_DIR") else Path.cwd()/"logs"
    setup_logging(log_root,level=logging.INFO,console=True)
    logging.info("---- [初始化] 启动服务")
    # 初始化 DB 池
    try:
        db.init_pool(config.DB_CONFIG)
    except Exception as e:
        logging.info("db.init_pool 失败, 继续尝试后操作: ",e)

    # 启动上传线程
    upl = uploader.Uploader()
    upl.start()

    stop_event = threading.Event()
    watcher_thread = threading.Thread(target = read_excel.slotfile_watcher_loop, args=(stop_event,),daemon=True, name="SlotfileWatcher")
    watcher_thread.start()
    # 优雅退出信号处理
    def handle_stop(signum, frame):
        logging.info("收到停止信号 %s, 准备退出", signum)
        upl.stop()
        stop_event.set()
        watcher_thread.join(timeout=3)
        sys.exit(0)
        
    signal.signal(signal.SIGINT, handle_stop)
    signal.signal(signal.SIGTERM, handle_stop)

    # 启动 FTP（这个会阻塞）
    ftp_handler.start_ftp_server()

if __name__ == "__main__":
    main()