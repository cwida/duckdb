from bs4 import BeautifulSoup
import requests
import re


pages = [
    'avg',
    'count',
    'max',
    'min',
    'stdev',
    'sum',
    'var',
    'cume-dist',
    'first-value',
    'last-value',
    'lag',
    'lead',
    'percent-rank',
    'dense-rank',
    'ntile',
    'rank',
    'row-number',
]

# crash!
# url = 'https://docs.microsoft.com/en-us/sql/t-sql/functions/first-value-transact-sql?view=sql-server-2017'
url = 'https://docs.microsoft.com/en-us/sql/t-sql/functions/%s-transact-sql?view=sql-server-2017'


def transform_result_set(tblstr):
    # find the row with the --- --- etc that indicates col count
    cols = len(re.findall(r"-{3,}( |\n)", tblstr))
    if cols < 1:
        return ""
    print("REQUIRE(result->ColumnCount() == %d);" % cols)
    lineiterator = iter(tblstr.splitlines())
    in_data = False
    result = []
    for c in range(0, cols):
        result.append([])

    for line in lineiterator:
        if '---' in line:
            in_data = True
            continue
        if not in_data:
            continue
        if 'row(s) affected' in line:
            continue
        if re.match(r"^\s*$", line):
            continue

        #  now we have a real data line, split by space and trim
        fields = re.split(r"\s{2,}", line)
        if len(fields) < cols:
            raise ValueError('Not enough fields')

        # print("// ", end='')
        for c in range(0, cols):
            f = fields[c].strip()
            if f == '':
                raise ValueError('Empty field')
            if re.match(r"^\d+[\d,]*\.\d+$", f):
                f = f.replace(',', '')
            # 	print(f + '\t', end='')
            needs_quotes = False
            try:
                float(f)
            except ValueError:
                needs_quotes = True

            if f == "NULL":
                f = 'Value()'
                needs_quotes = False

            if needs_quotes:
                f = '"%s"' % f
            result[c].append(f)
        # print()

    for c in range(0, cols):
        print('REQUIRE(CHECK_COLUMN(result, %d, {%s}));' % (c, ','.join(result[c])))


for p in pages:
    r = requests.get(url % p)
    print('\n\n// FROM %s\n' % url % p)
    soup = BeautifulSoup(r.content, 'html.parser')
    look_for_answer = False

    for code in soup.find_all('code'):
        classes = code.get('class')
        text = code.get_text()
        if text.count('\n') < 2:
            continue
        if 'SELECT ' in text and 'FROM ' in text:
            if 'dbo.' in text or 'sys.' in text:
                continue
            query = text.strip()
            query = re.sub(r"(^|\n)(GO|USE|DECLARE).*", "", query)
            query = query.replace('\n', ' ')
            query = re.sub(r"\s+", " ", query)

            print('\n\nresult = con.Query("%s");\nREQUIRE(!result->HasError());' % query.replace('"', '\\"'))

            look_for_answer = True
        elif look_for_answer:
            # print('-- ' + text.replace('\n', '\n-- ') + '\n')
            transform_result_set(text)
            look_for_answer = False
