import duckdb
import os
import sys

try:
    import pyarrow
    import pyarrow.parquet

    can_run = True
except:
    can_run = False


def generate_header(f):
    f.write(
        '''# name: test/parquet/test_parquet_reader.test
# description: Test Parquet Reader with files on data/parquet-testing
# group: [parquet]

require parquet

'''
    )


def get_files():
    files_path = []
    path = os.path.dirname(os.path.realpath(__file__))
    path = os.path.join(path, '..', '..')
    os.chdir(path)
    path = os.path.join('data', 'parquet-testing')
    for root, dirs, files in os.walk(path):
        for file in files:
            if file.endswith(".parquet"):
                files_path.append(os.path.join(root, file))
    return files_path


def get_duckdb_answer(file_path):
    answer = []
    try:
        answer = duckdb.query("SELECT * FROM parquet_scan('" + file_path + "') limit 50").fetchall()
    except Exception as e:
        print(e)
        answer = 'fail'
    return answer


def get_arrow_answer(file_path):
    answer = []
    try:
        arrow = pyarrow.parquet.read_table(file_path)
        duck_rel = duckdb.from_arrow(arrow).limit(50)
        answer = duck_rel.fetchall()
        return answer
    except:
        return 'fail'


def check_result(duckdb_result, arrow_result):
    if arrow_result == 'fail':
        return 'skip'
    if duckdb_result == 'fail':
        return 'fail'
    if duckdb_result != arrow_result:
        return 'fail'
    return 'pass'


def sanitize_string(s):
    return str(s).replace('None', 'NULL').replace("b'", "").replace("'", "")


def result_to_string(arrow_result):
    result = ''
    for row_idx in range(len(arrow_result)):
        for col_idx in range(len(arrow_result[0])):
            value = arrow_result[row_idx][col_idx]
            if isinstance(value, dict):
                items = [f"'{k}': {sanitize_string(v)}" for k, v in value.items()]  # no quotes
                value = "{" + ", ".join(items) + "}"
                print(type(value), value)
            else:
                value = sanitize_string(value)
            result += value + "\t"
        result += "\n"
    result += "\n"
    return result


def generate_parquet_test_body(result, arrow_result, file_path):
    columns = 'I' * len(arrow_result[0])
    test_body = "query " + columns + "\n"
    test_body += "SELECT * FROM parquet_scan('" + file_path + "') limit 50 \n"
    test_body += "----\n"
    test_body += result_to_string(arrow_result)
    return test_body


def generate_test(file_path):
    duckdb_result = get_duckdb_answer(file_path)
    arrow_result = get_arrow_answer(file_path)
    result = check_result(duckdb_result, arrow_result)
    test_body = ""
    if result == 'skip':
        return
    if result == 'fail':
        test_body += "mode skip \n\n"
        test_body += generate_parquet_test_body(result, arrow_result, file_path)
        test_body += "mode unskip \n\n"
    else:
        test_body += generate_parquet_test_body(result, duckdb_result, file_path)
    return test_body


def generate_body(f):
    files_path = get_files()
    for file in files_path:
        print(file)
        test_body = generate_test(file)
        if test_body != None:
            f.write(test_body)


f = open("test_parquet_reader.test", "w")

generate_header(f)
generate_body(f)


f.close()
