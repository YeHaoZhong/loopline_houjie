import mysql.connector
db = mysql.connector.connect(
    host = "localhost",
    user = "root",
    password = "123456",
    database = "loopline_byplc"
)

def ensure_insert(scan_code):
    try:
        sql = "SELECT path FROM pic WHERE code = %s"
        values = (scan_code,)
        cursor.execute(sql,values)
        results = cursor.fetchall()
        if len(results) == 0:
            print("results = ",results)
            return True
        else:
            print("results = ",results)
            return False
    except Exception as e:
        print("[错误]查询图片,单号= [",scan_code,"], 异常信息:",e)
        return True

if __name__ == "__main__":
    cursor = db.cursor()
    ensure_insert('test')