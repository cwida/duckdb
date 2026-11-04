# fmt: off

import pytest
from conftest import ShellTest
import re

lineitem_source = "lineitem"
all_types_source = "all_types"

# the expected results can be generated using the below function
# e.g. generate_results('build/reldebug/duckdb')
def generate_results(cli_binary, target_file_name='expected_output.txt', sources = ['lineitem', 'all_types'], modes = ['ascii', 'box', 'column', 'csv', 'duckbox', 'html', 'insert', 'json', 'jsonlines', 'latex', 'line', 'list', 'markdown', 'quote', 'table', 'tabs', 'tcl']):
    import subprocess
    with open(target_file_name, 'w+') as f:
        for source in sources:
            if source == 'lineitem':
               query = "SELECT * FROM 'data/parquet-testing/lineitem-top10000.gzip.parquet' LIMIT 10"
            elif source == "all_types":
                query = "SELECT * REPLACE (varchar.replace(chr(0), '\\0') AS varchar) FROM test_all_types()"
            for mode in modes:
                result =subprocess.run([cli_binary, '-c', "SET TimeZone='UTC'", '-c', f'.mode {mode}', '-c', query], capture_output=True, text=True)
                quotes = '"""' if (result.stdout[0] == '\'' or result.stdout[-1] == '\'' or "'''" in result.stdout) else "'''"
                f.write(f'expected_results[{source}_source]["{mode}"] = {quotes}')
                f.write(result.stdout.replace('\\', '\\\\'))
                f.write(f"{quotes}\n\n")

@pytest.mark.parametrize("mode", ['ascii', 'box', 'column', 'csv', 'duckbox', 'html', 'insert', 'json', 'jsonlines', 'latex', 'line', 'list', 'markdown', 'quote', 'table', 'tabs', 'tcl'])
@pytest.mark.parametrize("source", ["lineitem", "all_types"])
def test_mode_regression(shell, mode, source):
    if source == 'lineitem':
        query = "SELECT * FROM 'data/parquet-testing/lineitem-top10000.gzip.parquet' LIMIT 10"
    elif source == "all_types":
         query = "SELECT * REPLACE (varchar.replace(chr(0), '\\0') AS varchar) FROM test_all_types()"
    test = (
        ShellTest(shell)
        .statement("SET TimeZone='UTC'")
        .statement(f".mode {mode}")
        .statement(f"{query}")
    )

    result = test.run()
    lines = [x.strip() for x in re.split('[\n,]', get_expected_output(source, mode))]
    for line in lines:
        if len(line) == 0:
            continue
        result.check_stdout(line)


expected_results = {}
expected_results[lineitem_source] = {}
expected_results[all_types_source] = {}

expected_results[lineitem_source]['ascii'] = '''l_orderkey
l_partkey
l_suppkey
l_linenumber
l_quantity
l_extendedprice
l_discount
l_tax
l_returnflag
l_linestatus
l_shipdate
l_commitdate
l_receiptdate
l_shipinstruct
l_shipmode
l_comment
1
155190
7706
1
17
21168.23
0.04
0.02
N
O
1996-03-13
1996-02-12
1996-03-22
DELIVER IN PERSON
TRUCK
egular courts above the
1
67310
7311
2
36
45983.16
0.09
0.06
N
O
1996-04-12
1996-02-28
1996-04-20
TAKE BACK RETURN
MAIL
ly final dependencies: slyly bold 
1
63700
3701
3
8
13309.6
0.1
0.02
N
O
1996-01-29
1996-03-05
1996-01-31
TAKE BACK RETURN
REG AIR
riously. regular, express dep
1
2132
4633
4
28
28955.64
0.09
0.06
N
O
1996-04-21
1996-03-30
1996-05-16
NONE
AIR
lites. fluffily even de
1
24027
1534
5
24
22824.48
0.1
0.04
N
O
1996-03-30
1996-03-14
1996-04-01
NONE
FOB
 pending foxes. slyly re
1
15635
638
6
32
49620.16
0.07
0.02
N
O
1996-01-30
1996-02-07
1996-02-03
DELIVER IN PERSON
MAIL
arefully slyly ex
2
106170
1191
1
38
44694.46
0.0
0.05
N
O
1997-01-28
1997-01-14
1997-02-02
TAKE BACK RETURN
RAIL
ven requests. deposits breach a
3
4297
1798
1
45
54058.05
0.06
0.0
R
F
1994-02-02
1994-01-04
1994-02-23
NONE
AIR
ongside of the furiously brave acco
3
19036
6540
2
49
46796.47
0.1
0.0
R
F
1993-11-09
1993-12-20
1993-11-24
TAKE BACK RETURN
RAIL
 unusual accounts. eve
3
128449
3474
3
27
39890.88
0.06
0.07
A
F
1994-01-16
1993-11-22
1994-01-23
DELIVER IN PERSON
SHIP
nal foxes wake. 
'''

expected_results[lineitem_source]['box'] = '''┌────────────┬───────────┬───────────┬──────────────┬────────────┬─────────────────┬────────────┬───────┬──────────────┬──────────────┬────────────┬──────────────┬───────────────┬───────────────────┬────────────┬─────────────────────────────────────┐
│ l_orderkey │ l_partkey │ l_suppkey │ l_linenumber │ l_quantity │ l_extendedprice │ l_discount │ l_tax │ l_returnflag │ l_linestatus │ l_shipdate │ l_commitdate │ l_receiptdate │  l_shipinstruct   │ l_shipmode │              l_comment              │
├────────────┼───────────┼───────────┼──────────────┼────────────┼─────────────────┼────────────┼───────┼──────────────┼──────────────┼────────────┼──────────────┼───────────────┼───────────────────┼────────────┼─────────────────────────────────────┤
│ 1          │ 155190    │ 7706      │ 1            │ 17         │ 21168.23        │ 0.04       │ 0.02  │ N            │ O            │ 1996-03-13 │ 1996-02-12   │ 1996-03-22    │ DELIVER IN PERSON │ TRUCK      │ egular courts above the             │
│ 1          │ 67310     │ 7311      │ 2            │ 36         │ 45983.16        │ 0.09       │ 0.06  │ N            │ O            │ 1996-04-12 │ 1996-02-28   │ 1996-04-20    │ TAKE BACK RETURN  │ MAIL       │ ly final dependencies: slyly bold   │
│ 1          │ 63700     │ 3701      │ 3            │ 8          │ 13309.6         │ 0.1        │ 0.02  │ N            │ O            │ 1996-01-29 │ 1996-03-05   │ 1996-01-31    │ TAKE BACK RETURN  │ REG AIR    │ riously. regular, express dep       │
│ 1          │ 2132      │ 4633      │ 4            │ 28         │ 28955.64        │ 0.09       │ 0.06  │ N            │ O            │ 1996-04-21 │ 1996-03-30   │ 1996-05-16    │ NONE              │ AIR        │ lites. fluffily even de             │
│ 1          │ 24027     │ 1534      │ 5            │ 24         │ 22824.48        │ 0.1        │ 0.04  │ N            │ O            │ 1996-03-30 │ 1996-03-14   │ 1996-04-01    │ NONE              │ FOB        │  pending foxes. slyly re            │
│ 1          │ 15635     │ 638       │ 6            │ 32         │ 49620.16        │ 0.07       │ 0.02  │ N            │ O            │ 1996-01-30 │ 1996-02-07   │ 1996-02-03    │ DELIVER IN PERSON │ MAIL       │ arefully slyly ex                   │
│ 2          │ 106170    │ 1191      │ 1            │ 38         │ 44694.46        │ 0.0        │ 0.05  │ N            │ O            │ 1997-01-28 │ 1997-01-14   │ 1997-02-02    │ TAKE BACK RETURN  │ RAIL       │ ven requests. deposits breach a     │
│ 3          │ 4297      │ 1798      │ 1            │ 45         │ 54058.05        │ 0.06       │ 0.0   │ R            │ F            │ 1994-02-02 │ 1994-01-04   │ 1994-02-23    │ NONE              │ AIR        │ ongside of the furiously brave acco │
│ 3          │ 19036     │ 6540      │ 2            │ 49         │ 46796.47        │ 0.1        │ 0.0   │ R            │ F            │ 1993-11-09 │ 1993-12-20   │ 1993-11-24    │ TAKE BACK RETURN  │ RAIL       │  unusual accounts. eve              │
│ 3          │ 128449    │ 3474      │ 3            │ 27         │ 39890.88        │ 0.06       │ 0.07  │ A            │ F            │ 1994-01-16 │ 1993-11-22   │ 1994-01-23    │ DELIVER IN PERSON │ SHIP       │ nal foxes wake.                     │
└────────────┴───────────┴───────────┴──────────────┴────────────┴─────────────────┴────────────┴───────┴──────────────┴──────────────┴────────────┴──────────────┴───────────────┴───────────────────┴────────────┴─────────────────────────────────────┘
'''

expected_results[lineitem_source]['column'] = '''l_orderkey  l_partkey  l_suppkey  l_linenumber  l_quantity  l_extendedprice  l_discount  l_tax  l_returnflag  l_linestatus  l_shipdate  l_commitdate  l_receiptdate  l_shipinstruct     l_shipmode  l_comment                          
----------  ---------  ---------  ------------  ----------  ---------------  ----------  -----  ------------  ------------  ----------  ------------  -------------  -----------------  ----------  -----------------------------------
1           155190     7706       1             17          21168.23         0.04        0.02   N             O             1996-03-13  1996-02-12    1996-03-22     DELIVER IN PERSON  TRUCK       egular courts above the            
1           67310      7311       2             36          45983.16         0.09        0.06   N             O             1996-04-12  1996-02-28    1996-04-20     TAKE BACK RETURN   MAIL        ly final dependencies: slyly bold  
1           63700      3701       3             8           13309.6          0.1         0.02   N             O             1996-01-29  1996-03-05    1996-01-31     TAKE BACK RETURN   REG AIR     riously. regular, express dep      
1           2132       4633       4             28          28955.64         0.09        0.06   N             O             1996-04-21  1996-03-30    1996-05-16     NONE               AIR         lites. fluffily even de            
1           24027      1534       5             24          22824.48         0.1         0.04   N             O             1996-03-30  1996-03-14    1996-04-01     NONE               FOB          pending foxes. slyly re           
1           15635      638        6             32          49620.16         0.07        0.02   N             O             1996-01-30  1996-02-07    1996-02-03     DELIVER IN PERSON  MAIL        arefully slyly ex                  
2           106170     1191       1             38          44694.46         0.0         0.05   N             O             1997-01-28  1997-01-14    1997-02-02     TAKE BACK RETURN   RAIL        ven requests. deposits breach a    
3           4297       1798       1             45          54058.05         0.06        0.0    R             F             1994-02-02  1994-01-04    1994-02-23     NONE               AIR         ongside of the furiously brave acco
3           19036      6540       2             49          46796.47         0.1         0.0    R             F             1993-11-09  1993-12-20    1993-11-24     TAKE BACK RETURN   RAIL         unusual accounts. eve             
3           128449     3474       3             27          39890.88         0.06        0.07   A             F             1994-01-16  1993-11-22    1994-01-23     DELIVER IN PERSON  SHIP        nal foxes wake.                    
'''

expected_results[lineitem_source]['csv'] = '''l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment
1,155190,7706,1,17,21168.23,0.04,0.02,N,O,1996-03-13,1996-02-12,1996-03-22,DELIVER IN PERSON,TRUCK,egular courts above the
1,67310,7311,2,36,45983.16,0.09,0.06,N,O,1996-04-12,1996-02-28,1996-04-20,TAKE BACK RETURN,MAIL,ly final dependencies: slyly bold 
1,63700,3701,3,8,13309.6,0.1,0.02,N,O,1996-01-29,1996-03-05,1996-01-31,TAKE BACK RETURN,REG AIR,"riously. regular, express dep"
1,2132,4633,4,28,28955.64,0.09,0.06,N,O,1996-04-21,1996-03-30,1996-05-16,NONE,AIR,lites. fluffily even de
1,24027,1534,5,24,22824.48,0.1,0.04,N,O,1996-03-30,1996-03-14,1996-04-01,NONE,FOB, pending foxes. slyly re
1,15635,638,6,32,49620.16,0.07,0.02,N,O,1996-01-30,1996-02-07,1996-02-03,DELIVER IN PERSON,MAIL,arefully slyly ex
2,106170,1191,1,38,44694.46,0.0,0.05,N,O,1997-01-28,1997-01-14,1997-02-02,TAKE BACK RETURN,RAIL,ven requests. deposits breach a
3,4297,1798,1,45,54058.05,0.06,0.0,R,F,1994-02-02,1994-01-04,1994-02-23,NONE,AIR,ongside of the furiously brave acco
3,19036,6540,2,49,46796.47,0.1,0.0,R,F,1993-11-09,1993-12-20,1993-11-24,TAKE BACK RETURN,RAIL, unusual accounts. eve
3,128449,3474,3,27,39890.88,0.06,0.07,A,F,1994-01-16,1993-11-22,1994-01-23,DELIVER IN PERSON,SHIP,nal foxes wake. 
'''

expected_results[lineitem_source]['duckbox'] = '''┌────────────┬───────────┬───────────┬──────────────┬────────────┬─────────────────┬────────────┬────────┬──────────────┬──────────────┬────────────┬──────────────┬───────────────┬───────────────────┬────────────┬─────────────────────────────────────┐
│ l_orderkey │ l_partkey │ l_suppkey │ l_linenumber │ l_quantity │ l_extendedprice │ l_discount │ l_tax  │ l_returnflag │ l_linestatus │ l_shipdate │ l_commitdate │ l_receiptdate │  l_shipinstruct   │ l_shipmode │              l_comment              │
│   int64    │   int64   │   int64   │    int32     │   int32    │     double      │   double   │ double │   varchar    │   varchar    │  varchar   │   varchar    │    varchar    │      varchar      │  varchar   │               varchar               │
├────────────┼───────────┼───────────┼──────────────┼────────────┼─────────────────┼────────────┼────────┼──────────────┼──────────────┼────────────┼──────────────┼───────────────┼───────────────────┼────────────┼─────────────────────────────────────┤
│          1 │    155190 │      7706 │            1 │         17 │        21168.23 │       0.04 │   0.02 │ N            │ O            │ 1996-03-13 │ 1996-02-12   │ 1996-03-22    │ DELIVER IN PERSON │ TRUCK      │ egular courts above the             │
│          1 │     67310 │      7311 │            2 │         36 │        45983.16 │       0.09 │   0.06 │ N            │ O            │ 1996-04-12 │ 1996-02-28   │ 1996-04-20    │ TAKE BACK RETURN  │ MAIL       │ ly final dependencies: slyly bold   │
│          1 │     63700 │      3701 │            3 │          8 │         13309.6 │        0.1 │   0.02 │ N            │ O            │ 1996-01-29 │ 1996-03-05   │ 1996-01-31    │ TAKE BACK RETURN  │ REG AIR    │ riously. regular, express dep       │
│          1 │      2132 │      4633 │            4 │         28 │        28955.64 │       0.09 │   0.06 │ N            │ O            │ 1996-04-21 │ 1996-03-30   │ 1996-05-16    │ NONE              │ AIR        │ lites. fluffily even de             │
│          1 │     24027 │      1534 │            5 │         24 │        22824.48 │        0.1 │   0.04 │ N            │ O            │ 1996-03-30 │ 1996-03-14   │ 1996-04-01    │ NONE              │ FOB        │  pending foxes. slyly re            │
│          1 │     15635 │       638 │            6 │         32 │        49620.16 │       0.07 │   0.02 │ N            │ O            │ 1996-01-30 │ 1996-02-07   │ 1996-02-03    │ DELIVER IN PERSON │ MAIL       │ arefully slyly ex                   │
│          2 │    106170 │      1191 │            1 │         38 │        44694.46 │        0.0 │   0.05 │ N            │ O            │ 1997-01-28 │ 1997-01-14   │ 1997-02-02    │ TAKE BACK RETURN  │ RAIL       │ ven requests. deposits breach a     │
│          3 │      4297 │      1798 │            1 │         45 │        54058.05 │       0.06 │    0.0 │ R            │ F            │ 1994-02-02 │ 1994-01-04   │ 1994-02-23    │ NONE              │ AIR        │ ongside of the furiously brave acco │
│          3 │     19036 │      6540 │            2 │         49 │        46796.47 │        0.1 │    0.0 │ R            │ F            │ 1993-11-09 │ 1993-12-20   │ 1993-11-24    │ TAKE BACK RETURN  │ RAIL       │  unusual accounts. eve              │
│          3 │    128449 │      3474 │            3 │         27 │        39890.88 │       0.06 │   0.07 │ A            │ F            │ 1994-01-16 │ 1993-11-22   │ 1994-01-23    │ DELIVER IN PERSON │ SHIP       │ nal foxes wake.                     │
└────────────┴───────────┴───────────┴──────────────┴────────────┴─────────────────┴────────────┴────────┴──────────────┴──────────────┴────────────┴──────────────┴───────────────┴───────────────────┴────────────┴─────────────────────────────────────┘
  10 rows                                                                                                                                                                                                                                      16 columns
'''

expected_results[lineitem_source]['html'] = '''<tr><th>l_orderkey</th>
<th>l_partkey</th>
<th>l_suppkey</th>
<th>l_linenumber</th>
<th>l_quantity</th>
<th>l_extendedprice</th>
<th>l_discount</th>
<th>l_tax</th>
<th>l_returnflag</th>
<th>l_linestatus</th>
<th>l_shipdate</th>
<th>l_commitdate</th>
<th>l_receiptdate</th>
<th>l_shipinstruct</th>
<th>l_shipmode</th>
<th>l_comment</th>
</tr>
<tr><td>1</td>
<td>155190</td>
<td>7706</td>
<td>1</td>
<td>17</td>
<td>21168.23</td>
<td>0.04</td>
<td>0.02</td>
<td>N</td>
<td>O</td>
<td>1996-03-13</td>
<td>1996-02-12</td>
<td>1996-03-22</td>
<td>DELIVER IN PERSON</td>
<td>TRUCK</td>
<td>egular courts above the</td>
</tr>
<tr><td>1</td>
<td>67310</td>
<td>7311</td>
<td>2</td>
<td>36</td>
<td>45983.16</td>
<td>0.09</td>
<td>0.06</td>
<td>N</td>
<td>O</td>
<td>1996-04-12</td>
<td>1996-02-28</td>
<td>1996-04-20</td>
<td>TAKE BACK RETURN</td>
<td>MAIL</td>
<td>ly final dependencies: slyly bold </td>
</tr>
<tr><td>1</td>
<td>63700</td>
<td>3701</td>
<td>3</td>
<td>8</td>
<td>13309.6</td>
<td>0.1</td>
<td>0.02</td>
<td>N</td>
<td>O</td>
<td>1996-01-29</td>
<td>1996-03-05</td>
<td>1996-01-31</td>
<td>TAKE BACK RETURN</td>
<td>REG AIR</td>
<td>riously. regular, express dep</td>
</tr>
<tr><td>1</td>
<td>2132</td>
<td>4633</td>
<td>4</td>
<td>28</td>
<td>28955.64</td>
<td>0.09</td>
<td>0.06</td>
<td>N</td>
<td>O</td>
<td>1996-04-21</td>
<td>1996-03-30</td>
<td>1996-05-16</td>
<td>NONE</td>
<td>AIR</td>
<td>lites. fluffily even de</td>
</tr>
<tr><td>1</td>
<td>24027</td>
<td>1534</td>
<td>5</td>
<td>24</td>
<td>22824.48</td>
<td>0.1</td>
<td>0.04</td>
<td>N</td>
<td>O</td>
<td>1996-03-30</td>
<td>1996-03-14</td>
<td>1996-04-01</td>
<td>NONE</td>
<td>FOB</td>
<td> pending foxes. slyly re</td>
</tr>
<tr><td>1</td>
<td>15635</td>
<td>638</td>
<td>6</td>
<td>32</td>
<td>49620.16</td>
<td>0.07</td>
<td>0.02</td>
<td>N</td>
<td>O</td>
<td>1996-01-30</td>
<td>1996-02-07</td>
<td>1996-02-03</td>
<td>DELIVER IN PERSON</td>
<td>MAIL</td>
<td>arefully slyly ex</td>
</tr>
<tr><td>2</td>
<td>106170</td>
<td>1191</td>
<td>1</td>
<td>38</td>
<td>44694.46</td>
<td>0.0</td>
<td>0.05</td>
<td>N</td>
<td>O</td>
<td>1997-01-28</td>
<td>1997-01-14</td>
<td>1997-02-02</td>
<td>TAKE BACK RETURN</td>
<td>RAIL</td>
<td>ven requests. deposits breach a</td>
</tr>
<tr><td>3</td>
<td>4297</td>
<td>1798</td>
<td>1</td>
<td>45</td>
<td>54058.05</td>
<td>0.06</td>
<td>0.0</td>
<td>R</td>
<td>F</td>
<td>1994-02-02</td>
<td>1994-01-04</td>
<td>1994-02-23</td>
<td>NONE</td>
<td>AIR</td>
<td>ongside of the furiously brave acco</td>
</tr>
<tr><td>3</td>
<td>19036</td>
<td>6540</td>
<td>2</td>
<td>49</td>
<td>46796.47</td>
<td>0.1</td>
<td>0.0</td>
<td>R</td>
<td>F</td>
<td>1993-11-09</td>
<td>1993-12-20</td>
<td>1993-11-24</td>
<td>TAKE BACK RETURN</td>
<td>RAIL</td>
<td> unusual accounts. eve</td>
</tr>
<tr><td>3</td>
<td>128449</td>
<td>3474</td>
<td>3</td>
<td>27</td>
<td>39890.88</td>
<td>0.06</td>
<td>0.07</td>
<td>A</td>
<td>F</td>
<td>1994-01-16</td>
<td>1993-11-22</td>
<td>1994-01-23</td>
<td>DELIVER IN PERSON</td>
<td>SHIP</td>
<td>nal foxes wake. </td>
</tr>
'''

expected_results[lineitem_source]['insert'] = '''INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(1,155190,7706,1,17,21168.23,0.04,0.02,'N','O','1996-03-13','1996-02-12','1996-03-22','DELIVER IN PERSON','TRUCK','egular courts above the');
INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(1,67310,7311,2,36,45983.16,0.09,0.06,'N','O','1996-04-12','1996-02-28','1996-04-20','TAKE BACK RETURN','MAIL','ly final dependencies: slyly bold ');
INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(1,63700,3701,3,8,13309.6,0.1,0.02,'N','O','1996-01-29','1996-03-05','1996-01-31','TAKE BACK RETURN','REG AIR','riously. regular, express dep');
INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(1,2132,4633,4,28,28955.64,0.09,0.06,'N','O','1996-04-21','1996-03-30','1996-05-16','NONE','AIR','lites. fluffily even de');
INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(1,24027,1534,5,24,22824.48,0.1,0.04,'N','O','1996-03-30','1996-03-14','1996-04-01','NONE','FOB',' pending foxes. slyly re');
INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(1,15635,638,6,32,49620.16,0.07,0.02,'N','O','1996-01-30','1996-02-07','1996-02-03','DELIVER IN PERSON','MAIL','arefully slyly ex');
INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(2,106170,1191,1,38,44694.46,0.0,0.05,'N','O','1997-01-28','1997-01-14','1997-02-02','TAKE BACK RETURN','RAIL','ven requests. deposits breach a');
INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(3,4297,1798,1,45,54058.05,0.06,0.0,'R','F','1994-02-02','1994-01-04','1994-02-23','NONE','AIR','ongside of the furiously brave acco');
INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(3,19036,6540,2,49,46796.47,0.1,0.0,'R','F','1993-11-09','1993-12-20','1993-11-24','TAKE BACK RETURN','RAIL',' unusual accounts. eve');
INSERT INTO "table"(l_orderkey,l_partkey,l_suppkey,l_linenumber,l_quantity,l_extendedprice,l_discount,l_tax,l_returnflag,l_linestatus,l_shipdate,l_commitdate,l_receiptdate,l_shipinstruct,l_shipmode,l_comment) VALUES(3,128449,3474,3,27,39890.88,0.06,0.07,'A','F','1994-01-16','1993-11-22','1994-01-23','DELIVER IN PERSON','SHIP','nal foxes wake. ');
'''

expected_results[lineitem_source]['json'] = '''[{"l_orderkey":1,"l_partkey":155190,"l_suppkey":7706,"l_linenumber":1,"l_quantity":17,"l_extendedprice":21168.23,"l_discount":0.04,"l_tax":0.02,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-03-13","l_commitdate":"1996-02-12","l_receiptdate":"1996-03-22","l_shipinstruct":"DELIVER IN PERSON","l_shipmode":"TRUCK","l_comment":"egular courts above the"},
{"l_orderkey":1,"l_partkey":67310,"l_suppkey":7311,"l_linenumber":2,"l_quantity":36,"l_extendedprice":45983.16,"l_discount":0.09,"l_tax":0.06,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-04-12","l_commitdate":"1996-02-28","l_receiptdate":"1996-04-20","l_shipinstruct":"TAKE BACK RETURN","l_shipmode":"MAIL","l_comment":"ly final dependencies: slyly bold "},
{"l_orderkey":1,"l_partkey":63700,"l_suppkey":3701,"l_linenumber":3,"l_quantity":8,"l_extendedprice":13309.6,"l_discount":0.1,"l_tax":0.02,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-01-29","l_commitdate":"1996-03-05","l_receiptdate":"1996-01-31","l_shipinstruct":"TAKE BACK RETURN","l_shipmode":"REG AIR","l_comment":"riously. regular, express dep"},
{"l_orderkey":1,"l_partkey":2132,"l_suppkey":4633,"l_linenumber":4,"l_quantity":28,"l_extendedprice":28955.64,"l_discount":0.09,"l_tax":0.06,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-04-21","l_commitdate":"1996-03-30","l_receiptdate":"1996-05-16","l_shipinstruct":"NONE","l_shipmode":"AIR","l_comment":"lites. fluffily even de"},
{"l_orderkey":1,"l_partkey":24027,"l_suppkey":1534,"l_linenumber":5,"l_quantity":24,"l_extendedprice":22824.48,"l_discount":0.1,"l_tax":0.04,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-03-30","l_commitdate":"1996-03-14","l_receiptdate":"1996-04-01","l_shipinstruct":"NONE","l_shipmode":"FOB","l_comment":" pending foxes. slyly re"},
{"l_orderkey":1,"l_partkey":15635,"l_suppkey":638,"l_linenumber":6,"l_quantity":32,"l_extendedprice":49620.16,"l_discount":0.07,"l_tax":0.02,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-01-30","l_commitdate":"1996-02-07","l_receiptdate":"1996-02-03","l_shipinstruct":"DELIVER IN PERSON","l_shipmode":"MAIL","l_comment":"arefully slyly ex"},
{"l_orderkey":2,"l_partkey":106170,"l_suppkey":1191,"l_linenumber":1,"l_quantity":38,"l_extendedprice":44694.46,"l_discount":0.0,"l_tax":0.05,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1997-01-28","l_commitdate":"1997-01-14","l_receiptdate":"1997-02-02","l_shipinstruct":"TAKE BACK RETURN","l_shipmode":"RAIL","l_comment":"ven requests. deposits breach a"},
{"l_orderkey":3,"l_partkey":4297,"l_suppkey":1798,"l_linenumber":1,"l_quantity":45,"l_extendedprice":54058.05,"l_discount":0.06,"l_tax":0.0,"l_returnflag":"R","l_linestatus":"F","l_shipdate":"1994-02-02","l_commitdate":"1994-01-04","l_receiptdate":"1994-02-23","l_shipinstruct":"NONE","l_shipmode":"AIR","l_comment":"ongside of the furiously brave acco"},
{"l_orderkey":3,"l_partkey":19036,"l_suppkey":6540,"l_linenumber":2,"l_quantity":49,"l_extendedprice":46796.47,"l_discount":0.1,"l_tax":0.0,"l_returnflag":"R","l_linestatus":"F","l_shipdate":"1993-11-09","l_commitdate":"1993-12-20","l_receiptdate":"1993-11-24","l_shipinstruct":"TAKE BACK RETURN","l_shipmode":"RAIL","l_comment":" unusual accounts. eve"},
{"l_orderkey":3,"l_partkey":128449,"l_suppkey":3474,"l_linenumber":3,"l_quantity":27,"l_extendedprice":39890.88,"l_discount":0.06,"l_tax":0.07,"l_returnflag":"A","l_linestatus":"F","l_shipdate":"1994-01-16","l_commitdate":"1993-11-22","l_receiptdate":"1994-01-23","l_shipinstruct":"DELIVER IN PERSON","l_shipmode":"SHIP","l_comment":"nal foxes wake. "}]
'''

expected_results[lineitem_source]['jsonlines'] = '''{"l_orderkey":1,"l_partkey":155190,"l_suppkey":7706,"l_linenumber":1,"l_quantity":17,"l_extendedprice":21168.23,"l_discount":0.04,"l_tax":0.02,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-03-13","l_commitdate":"1996-02-12","l_receiptdate":"1996-03-22","l_shipinstruct":"DELIVER IN PERSON","l_shipmode":"TRUCK","l_comment":"egular courts above the"}
{"l_orderkey":1,"l_partkey":67310,"l_suppkey":7311,"l_linenumber":2,"l_quantity":36,"l_extendedprice":45983.16,"l_discount":0.09,"l_tax":0.06,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-04-12","l_commitdate":"1996-02-28","l_receiptdate":"1996-04-20","l_shipinstruct":"TAKE BACK RETURN","l_shipmode":"MAIL","l_comment":"ly final dependencies: slyly bold "}
{"l_orderkey":1,"l_partkey":63700,"l_suppkey":3701,"l_linenumber":3,"l_quantity":8,"l_extendedprice":13309.6,"l_discount":0.1,"l_tax":0.02,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-01-29","l_commitdate":"1996-03-05","l_receiptdate":"1996-01-31","l_shipinstruct":"TAKE BACK RETURN","l_shipmode":"REG AIR","l_comment":"riously. regular, express dep"}
{"l_orderkey":1,"l_partkey":2132,"l_suppkey":4633,"l_linenumber":4,"l_quantity":28,"l_extendedprice":28955.64,"l_discount":0.09,"l_tax":0.06,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-04-21","l_commitdate":"1996-03-30","l_receiptdate":"1996-05-16","l_shipinstruct":"NONE","l_shipmode":"AIR","l_comment":"lites. fluffily even de"}
{"l_orderkey":1,"l_partkey":24027,"l_suppkey":1534,"l_linenumber":5,"l_quantity":24,"l_extendedprice":22824.48,"l_discount":0.1,"l_tax":0.04,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-03-30","l_commitdate":"1996-03-14","l_receiptdate":"1996-04-01","l_shipinstruct":"NONE","l_shipmode":"FOB","l_comment":" pending foxes. slyly re"}
{"l_orderkey":1,"l_partkey":15635,"l_suppkey":638,"l_linenumber":6,"l_quantity":32,"l_extendedprice":49620.16,"l_discount":0.07,"l_tax":0.02,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1996-01-30","l_commitdate":"1996-02-07","l_receiptdate":"1996-02-03","l_shipinstruct":"DELIVER IN PERSON","l_shipmode":"MAIL","l_comment":"arefully slyly ex"}
{"l_orderkey":2,"l_partkey":106170,"l_suppkey":1191,"l_linenumber":1,"l_quantity":38,"l_extendedprice":44694.46,"l_discount":0.0,"l_tax":0.05,"l_returnflag":"N","l_linestatus":"O","l_shipdate":"1997-01-28","l_commitdate":"1997-01-14","l_receiptdate":"1997-02-02","l_shipinstruct":"TAKE BACK RETURN","l_shipmode":"RAIL","l_comment":"ven requests. deposits breach a"}
{"l_orderkey":3,"l_partkey":4297,"l_suppkey":1798,"l_linenumber":1,"l_quantity":45,"l_extendedprice":54058.05,"l_discount":0.06,"l_tax":0.0,"l_returnflag":"R","l_linestatus":"F","l_shipdate":"1994-02-02","l_commitdate":"1994-01-04","l_receiptdate":"1994-02-23","l_shipinstruct":"NONE","l_shipmode":"AIR","l_comment":"ongside of the furiously brave acco"}
{"l_orderkey":3,"l_partkey":19036,"l_suppkey":6540,"l_linenumber":2,"l_quantity":49,"l_extendedprice":46796.47,"l_discount":0.1,"l_tax":0.0,"l_returnflag":"R","l_linestatus":"F","l_shipdate":"1993-11-09","l_commitdate":"1993-12-20","l_receiptdate":"1993-11-24","l_shipinstruct":"TAKE BACK RETURN","l_shipmode":"RAIL","l_comment":" unusual accounts. eve"}
{"l_orderkey":3,"l_partkey":128449,"l_suppkey":3474,"l_linenumber":3,"l_quantity":27,"l_extendedprice":39890.88,"l_discount":0.06,"l_tax":0.07,"l_returnflag":"A","l_linestatus":"F","l_shipdate":"1994-01-16","l_commitdate":"1993-11-22","l_receiptdate":"1994-01-23","l_shipinstruct":"DELIVER IN PERSON","l_shipmode":"SHIP","l_comment":"nal foxes wake. "}
'''

expected_results[lineitem_source]['latex'] = '''\\begin{tabular}{|rrrrrrrrllllllll|}
\\hline
l_orderkey & l_partkey & l_suppkey & l_linenumber & l_quantity & l_extendedprice & l_discount & l_tax & l_returnflag & l_linestatus & l_shipdate & l_commitdate & l_receiptdate &  l_shipinstruct   & l_shipmode &              l_comment              \\\\
\\hline
1          & 155190    & 7706      & 1            & 17         & 21168.23        & 0.04       & 0.02  & N            & O            & 1996-03-13 & 1996-02-12   & 1996-03-22    & DELIVER IN PERSON & TRUCK      & egular courts above the             \\\\
1          & 67310     & 7311      & 2            & 36         & 45983.16        & 0.09       & 0.06  & N            & O            & 1996-04-12 & 1996-02-28   & 1996-04-20    & TAKE BACK RETURN  & MAIL       & ly final dependencies: slyly bold   \\\\
1          & 63700     & 3701      & 3            & 8          & 13309.6         & 0.1        & 0.02  & N            & O            & 1996-01-29 & 1996-03-05   & 1996-01-31    & TAKE BACK RETURN  & REG AIR    & riously. regular, express dep       \\\\
1          & 2132      & 4633      & 4            & 28         & 28955.64        & 0.09       & 0.06  & N            & O            & 1996-04-21 & 1996-03-30   & 1996-05-16    & NONE              & AIR        & lites. fluffily even de             \\\\
1          & 24027     & 1534      & 5            & 24         & 22824.48        & 0.1        & 0.04  & N            & O            & 1996-03-30 & 1996-03-14   & 1996-04-01    & NONE              & FOB        &  pending foxes. slyly re            \\\\
1          & 15635     & 638       & 6            & 32         & 49620.16        & 0.07       & 0.02  & N            & O            & 1996-01-30 & 1996-02-07   & 1996-02-03    & DELIVER IN PERSON & MAIL       & arefully slyly ex                   \\\\
2          & 106170    & 1191      & 1            & 38         & 44694.46        & 0.0        & 0.05  & N            & O            & 1997-01-28 & 1997-01-14   & 1997-02-02    & TAKE BACK RETURN  & RAIL       & ven requests. deposits breach a     \\\\
3          & 4297      & 1798      & 1            & 45         & 54058.05        & 0.06       & 0.0   & R            & F            & 1994-02-02 & 1994-01-04   & 1994-02-23    & NONE              & AIR        & ongside of the furiously brave acco \\\\
3          & 19036     & 6540      & 2            & 49         & 46796.47        & 0.1        & 0.0   & R            & F            & 1993-11-09 & 1993-12-20   & 1993-11-24    & TAKE BACK RETURN  & RAIL       &  unusual accounts. eve              \\\\
3          & 128449    & 3474      & 3            & 27         & 39890.88        & 0.06       & 0.07  & A            & F            & 1994-01-16 & 1993-11-22   & 1994-01-23    & DELIVER IN PERSON & SHIP       & nal foxes wake.                     \\\\
\\hline
\\end{tabular}
'''

expected_results[lineitem_source]['line'] = '''     l_orderkey = 1
      l_partkey = 155190
      l_suppkey = 7706
   l_linenumber = 1
     l_quantity = 17
l_extendedprice = 21168.23
     l_discount = 0.04
          l_tax = 0.02
   l_returnflag = N
   l_linestatus = O
     l_shipdate = 1996-03-13
   l_commitdate = 1996-02-12
  l_receiptdate = 1996-03-22
 l_shipinstruct = DELIVER IN PERSON
     l_shipmode = TRUCK
      l_comment = egular courts above the

     l_orderkey = 1
      l_partkey = 67310
      l_suppkey = 7311
   l_linenumber = 2
     l_quantity = 36
l_extendedprice = 45983.16
     l_discount = 0.09
          l_tax = 0.06
   l_returnflag = N
   l_linestatus = O
     l_shipdate = 1996-04-12
   l_commitdate = 1996-02-28
  l_receiptdate = 1996-04-20
 l_shipinstruct = TAKE BACK RETURN
     l_shipmode = MAIL
      l_comment = ly final dependencies: slyly bold 

     l_orderkey = 1
      l_partkey = 63700
      l_suppkey = 3701
   l_linenumber = 3
     l_quantity = 8
l_extendedprice = 13309.6
     l_discount = 0.1
          l_tax = 0.02
   l_returnflag = N
   l_linestatus = O
     l_shipdate = 1996-01-29
   l_commitdate = 1996-03-05
  l_receiptdate = 1996-01-31
 l_shipinstruct = TAKE BACK RETURN
     l_shipmode = REG AIR
      l_comment = riously. regular, express dep

     l_orderkey = 1
      l_partkey = 2132
      l_suppkey = 4633
   l_linenumber = 4
     l_quantity = 28
l_extendedprice = 28955.64
     l_discount = 0.09
          l_tax = 0.06
   l_returnflag = N
   l_linestatus = O
     l_shipdate = 1996-04-21
   l_commitdate = 1996-03-30
  l_receiptdate = 1996-05-16
 l_shipinstruct = NONE
     l_shipmode = AIR
      l_comment = lites. fluffily even de

     l_orderkey = 1
      l_partkey = 24027
      l_suppkey = 1534
   l_linenumber = 5
     l_quantity = 24
l_extendedprice = 22824.48
     l_discount = 0.1
          l_tax = 0.04
   l_returnflag = N
   l_linestatus = O
     l_shipdate = 1996-03-30
   l_commitdate = 1996-03-14
  l_receiptdate = 1996-04-01
 l_shipinstruct = NONE
     l_shipmode = FOB
      l_comment =  pending foxes. slyly re

     l_orderkey = 1
      l_partkey = 15635
      l_suppkey = 638
   l_linenumber = 6
     l_quantity = 32
l_extendedprice = 49620.16
     l_discount = 0.07
          l_tax = 0.02
   l_returnflag = N
   l_linestatus = O
     l_shipdate = 1996-01-30
   l_commitdate = 1996-02-07
  l_receiptdate = 1996-02-03
 l_shipinstruct = DELIVER IN PERSON
     l_shipmode = MAIL
      l_comment = arefully slyly ex

     l_orderkey = 2
      l_partkey = 106170
      l_suppkey = 1191
   l_linenumber = 1
     l_quantity = 38
l_extendedprice = 44694.46
     l_discount = 0.0
          l_tax = 0.05
   l_returnflag = N
   l_linestatus = O
     l_shipdate = 1997-01-28
   l_commitdate = 1997-01-14
  l_receiptdate = 1997-02-02
 l_shipinstruct = TAKE BACK RETURN
     l_shipmode = RAIL
      l_comment = ven requests. deposits breach a

     l_orderkey = 3
      l_partkey = 4297
      l_suppkey = 1798
   l_linenumber = 1
     l_quantity = 45
l_extendedprice = 54058.05
     l_discount = 0.06
          l_tax = 0.0
   l_returnflag = R
   l_linestatus = F
     l_shipdate = 1994-02-02
   l_commitdate = 1994-01-04
  l_receiptdate = 1994-02-23
 l_shipinstruct = NONE
     l_shipmode = AIR
      l_comment = ongside of the furiously brave acco

     l_orderkey = 3
      l_partkey = 19036
      l_suppkey = 6540
   l_linenumber = 2
     l_quantity = 49
l_extendedprice = 46796.47
     l_discount = 0.1
          l_tax = 0.0
   l_returnflag = R
   l_linestatus = F
     l_shipdate = 1993-11-09
   l_commitdate = 1993-12-20
  l_receiptdate = 1993-11-24
 l_shipinstruct = TAKE BACK RETURN
     l_shipmode = RAIL
      l_comment =  unusual accounts. eve

     l_orderkey = 3
      l_partkey = 128449
      l_suppkey = 3474
   l_linenumber = 3
     l_quantity = 27
l_extendedprice = 39890.88
     l_discount = 0.06
          l_tax = 0.07
   l_returnflag = A
   l_linestatus = F
     l_shipdate = 1994-01-16
   l_commitdate = 1993-11-22
  l_receiptdate = 1994-01-23
 l_shipinstruct = DELIVER IN PERSON
     l_shipmode = SHIP
      l_comment = nal foxes wake. 
'''

expected_results[lineitem_source]['list'] = '''l_orderkey|l_partkey|l_suppkey|l_linenumber|l_quantity|l_extendedprice|l_discount|l_tax|l_returnflag|l_linestatus|l_shipdate|l_commitdate|l_receiptdate|l_shipinstruct|l_shipmode|l_comment
1|155190|7706|1|17|21168.23|0.04|0.02|N|O|1996-03-13|1996-02-12|1996-03-22|DELIVER IN PERSON|TRUCK|egular courts above the
1|67310|7311|2|36|45983.16|0.09|0.06|N|O|1996-04-12|1996-02-28|1996-04-20|TAKE BACK RETURN|MAIL|ly final dependencies: slyly bold 
1|63700|3701|3|8|13309.6|0.1|0.02|N|O|1996-01-29|1996-03-05|1996-01-31|TAKE BACK RETURN|REG AIR|riously. regular, express dep
1|2132|4633|4|28|28955.64|0.09|0.06|N|O|1996-04-21|1996-03-30|1996-05-16|NONE|AIR|lites. fluffily even de
1|24027|1534|5|24|22824.48|0.1|0.04|N|O|1996-03-30|1996-03-14|1996-04-01|NONE|FOB| pending foxes. slyly re
1|15635|638|6|32|49620.16|0.07|0.02|N|O|1996-01-30|1996-02-07|1996-02-03|DELIVER IN PERSON|MAIL|arefully slyly ex
2|106170|1191|1|38|44694.46|0.0|0.05|N|O|1997-01-28|1997-01-14|1997-02-02|TAKE BACK RETURN|RAIL|ven requests. deposits breach a
3|4297|1798|1|45|54058.05|0.06|0.0|R|F|1994-02-02|1994-01-04|1994-02-23|NONE|AIR|ongside of the furiously brave acco
3|19036|6540|2|49|46796.47|0.1|0.0|R|F|1993-11-09|1993-12-20|1993-11-24|TAKE BACK RETURN|RAIL| unusual accounts. eve
3|128449|3474|3|27|39890.88|0.06|0.07|A|F|1994-01-16|1993-11-22|1994-01-23|DELIVER IN PERSON|SHIP|nal foxes wake. 
'''

expected_results[lineitem_source]['markdown'] = '''| l_orderkey | l_partkey | l_suppkey | l_linenumber | l_quantity | l_extendedprice | l_discount | l_tax | l_returnflag | l_linestatus | l_shipdate | l_commitdate | l_receiptdate |  l_shipinstruct   | l_shipmode |              l_comment              |
|-----------:|----------:|----------:|-------------:|-----------:|----------------:|-----------:|------:|--------------|--------------|------------|--------------|---------------|-------------------|------------|-------------------------------------|
| 1          | 155190    | 7706      | 1            | 17         | 21168.23        | 0.04       | 0.02  | N            | O            | 1996-03-13 | 1996-02-12   | 1996-03-22    | DELIVER IN PERSON | TRUCK      | egular courts above the             |
| 1          | 67310     | 7311      | 2            | 36         | 45983.16        | 0.09       | 0.06  | N            | O            | 1996-04-12 | 1996-02-28   | 1996-04-20    | TAKE BACK RETURN  | MAIL       | ly final dependencies: slyly bold   |
| 1          | 63700     | 3701      | 3            | 8          | 13309.6         | 0.1        | 0.02  | N            | O            | 1996-01-29 | 1996-03-05   | 1996-01-31    | TAKE BACK RETURN  | REG AIR    | riously. regular, express dep       |
| 1          | 2132      | 4633      | 4            | 28         | 28955.64        | 0.09       | 0.06  | N            | O            | 1996-04-21 | 1996-03-30   | 1996-05-16    | NONE              | AIR        | lites. fluffily even de             |
| 1          | 24027     | 1534      | 5            | 24         | 22824.48        | 0.1        | 0.04  | N            | O            | 1996-03-30 | 1996-03-14   | 1996-04-01    | NONE              | FOB        |  pending foxes. slyly re            |
| 1          | 15635     | 638       | 6            | 32         | 49620.16        | 0.07       | 0.02  | N            | O            | 1996-01-30 | 1996-02-07   | 1996-02-03    | DELIVER IN PERSON | MAIL       | arefully slyly ex                   |
| 2          | 106170    | 1191      | 1            | 38         | 44694.46        | 0.0        | 0.05  | N            | O            | 1997-01-28 | 1997-01-14   | 1997-02-02    | TAKE BACK RETURN  | RAIL       | ven requests. deposits breach a     |
| 3          | 4297      | 1798      | 1            | 45         | 54058.05        | 0.06       | 0.0   | R            | F            | 1994-02-02 | 1994-01-04   | 1994-02-23    | NONE              | AIR        | ongside of the furiously brave acco |
| 3          | 19036     | 6540      | 2            | 49         | 46796.47        | 0.1        | 0.0   | R            | F            | 1993-11-09 | 1993-12-20   | 1993-11-24    | TAKE BACK RETURN  | RAIL       |  unusual accounts. eve              |
| 3          | 128449    | 3474      | 3            | 27         | 39890.88        | 0.06       | 0.07  | A            | F            | 1994-01-16 | 1993-11-22   | 1994-01-23    | DELIVER IN PERSON | SHIP       | nal foxes wake.                     |
'''

expected_results[lineitem_source]['quote'] = ''''l_orderkey','l_partkey','l_suppkey','l_linenumber','l_quantity','l_extendedprice','l_discount','l_tax','l_returnflag','l_linestatus','l_shipdate','l_commitdate','l_receiptdate','l_shipinstruct','l_shipmode','l_comment'
1,155190,7706,1,17,21168.23,0.04,0.02,'N','O','1996-03-13','1996-02-12','1996-03-22','DELIVER IN PERSON','TRUCK','egular courts above the'
1,67310,7311,2,36,45983.16,0.09,0.06,'N','O','1996-04-12','1996-02-28','1996-04-20','TAKE BACK RETURN','MAIL','ly final dependencies: slyly bold '
1,63700,3701,3,8,13309.6,0.1,0.02,'N','O','1996-01-29','1996-03-05','1996-01-31','TAKE BACK RETURN','REG AIR','riously. regular, express dep'
1,2132,4633,4,28,28955.64,0.09,0.06,'N','O','1996-04-21','1996-03-30','1996-05-16','NONE','AIR','lites. fluffily even de'
1,24027,1534,5,24,22824.48,0.1,0.04,'N','O','1996-03-30','1996-03-14','1996-04-01','NONE','FOB',' pending foxes. slyly re'
1,15635,638,6,32,49620.16,0.07,0.02,'N','O','1996-01-30','1996-02-07','1996-02-03','DELIVER IN PERSON','MAIL','arefully slyly ex'
2,106170,1191,1,38,44694.46,0.0,0.05,'N','O','1997-01-28','1997-01-14','1997-02-02','TAKE BACK RETURN','RAIL','ven requests. deposits breach a'
3,4297,1798,1,45,54058.05,0.06,0.0,'R','F','1994-02-02','1994-01-04','1994-02-23','NONE','AIR','ongside of the furiously brave acco'
3,19036,6540,2,49,46796.47,0.1,0.0,'R','F','1993-11-09','1993-12-20','1993-11-24','TAKE BACK RETURN','RAIL',' unusual accounts. eve'
3,128449,3474,3,27,39890.88,0.06,0.07,'A','F','1994-01-16','1993-11-22','1994-01-23','DELIVER IN PERSON','SHIP','nal foxes wake. '
'''

expected_results[lineitem_source]['table'] = '''+------------+-----------+-----------+--------------+------------+-----------------+------------+-------+--------------+--------------+------------+--------------+---------------+-------------------+------------+-------------------------------------+
| l_orderkey | l_partkey | l_suppkey | l_linenumber | l_quantity | l_extendedprice | l_discount | l_tax | l_returnflag | l_linestatus | l_shipdate | l_commitdate | l_receiptdate |  l_shipinstruct   | l_shipmode |              l_comment              |
+------------+-----------+-----------+--------------+------------+-----------------+------------+-------+--------------+--------------+------------+--------------+---------------+-------------------+------------+-------------------------------------+
| 1          | 155190    | 7706      | 1            | 17         | 21168.23        | 0.04       | 0.02  | N            | O            | 1996-03-13 | 1996-02-12   | 1996-03-22    | DELIVER IN PERSON | TRUCK      | egular courts above the             |
| 1          | 67310     | 7311      | 2            | 36         | 45983.16        | 0.09       | 0.06  | N            | O            | 1996-04-12 | 1996-02-28   | 1996-04-20    | TAKE BACK RETURN  | MAIL       | ly final dependencies: slyly bold   |
| 1          | 63700     | 3701      | 3            | 8          | 13309.6         | 0.1        | 0.02  | N            | O            | 1996-01-29 | 1996-03-05   | 1996-01-31    | TAKE BACK RETURN  | REG AIR    | riously. regular, express dep       |
| 1          | 2132      | 4633      | 4            | 28         | 28955.64        | 0.09       | 0.06  | N            | O            | 1996-04-21 | 1996-03-30   | 1996-05-16    | NONE              | AIR        | lites. fluffily even de             |
| 1          | 24027     | 1534      | 5            | 24         | 22824.48        | 0.1        | 0.04  | N            | O            | 1996-03-30 | 1996-03-14   | 1996-04-01    | NONE              | FOB        |  pending foxes. slyly re            |
| 1          | 15635     | 638       | 6            | 32         | 49620.16        | 0.07       | 0.02  | N            | O            | 1996-01-30 | 1996-02-07   | 1996-02-03    | DELIVER IN PERSON | MAIL       | arefully slyly ex                   |
| 2          | 106170    | 1191      | 1            | 38         | 44694.46        | 0.0        | 0.05  | N            | O            | 1997-01-28 | 1997-01-14   | 1997-02-02    | TAKE BACK RETURN  | RAIL       | ven requests. deposits breach a     |
| 3          | 4297      | 1798      | 1            | 45         | 54058.05        | 0.06       | 0.0   | R            | F            | 1994-02-02 | 1994-01-04   | 1994-02-23    | NONE              | AIR        | ongside of the furiously brave acco |
| 3          | 19036     | 6540      | 2            | 49         | 46796.47        | 0.1        | 0.0   | R            | F            | 1993-11-09 | 1993-12-20   | 1993-11-24    | TAKE BACK RETURN  | RAIL       |  unusual accounts. eve              |
| 3          | 128449    | 3474      | 3            | 27         | 39890.88        | 0.06       | 0.07  | A            | F            | 1994-01-16 | 1993-11-22   | 1994-01-23    | DELIVER IN PERSON | SHIP       | nal foxes wake.                     |
+------------+-----------+-----------+--------------+------------+-----------------+------------+-------+--------------+--------------+------------+--------------+---------------+-------------------+------------+-------------------------------------+
'''

expected_results[lineitem_source]['tabs'] = '''l_orderkey	l_partkey	l_suppkey	l_linenumber	l_quantity	l_extendedprice	l_discount	l_tax	l_returnflag	l_linestatus	l_shipdate	l_commitdate	l_receiptdate	l_shipinstruct	l_shipmode	l_comment
1	155190	7706	1	17	21168.23	0.04	0.02	N	O	1996-03-13	1996-02-12	1996-03-22	DELIVER IN PERSON	TRUCK	egular courts above the
1	67310	7311	2	36	45983.16	0.09	0.06	N	O	1996-04-12	1996-02-28	1996-04-20	TAKE BACK RETURN	MAIL	ly final dependencies: slyly bold 
1	63700	3701	3	8	13309.6	0.1	0.02	N	O	1996-01-29	1996-03-05	1996-01-31	TAKE BACK RETURN	REG AIR	riously. regular, express dep
1	2132	4633	4	28	28955.64	0.09	0.06	N	O	1996-04-21	1996-03-30	1996-05-16	NONE	AIR	lites. fluffily even de
1	24027	1534	5	24	22824.48	0.1	0.04	N	O	1996-03-30	1996-03-14	1996-04-01	NONE	FOB	 pending foxes. slyly re
1	15635	638	6	32	49620.16	0.07	0.02	N	O	1996-01-30	1996-02-07	1996-02-03	DELIVER IN PERSON	MAIL	arefully slyly ex
2	106170	1191	1	38	44694.46	0.0	0.05	N	O	1997-01-28	1997-01-14	1997-02-02	TAKE BACK RETURN	RAIL	ven requests. deposits breach a
3	4297	1798	1	45	54058.05	0.06	0.0	R	F	1994-02-02	1994-01-04	1994-02-23	NONE	AIR	ongside of the furiously brave acco
3	19036	6540	2	49	46796.47	0.1	0.0	R	F	1993-11-09	1993-12-20	1993-11-24	TAKE BACK RETURN	RAIL	 unusual accounts. eve
3	128449	3474	3	27	39890.88	0.06	0.07	A	F	1994-01-16	1993-11-22	1994-01-23	DELIVER IN PERSON	SHIP	nal foxes wake. 
'''

expected_results[lineitem_source]['tcl'] = '''"l_orderkey" "l_partkey" "l_suppkey" "l_linenumber" "l_quantity" "l_extendedprice" "l_discount" "l_tax" "l_returnflag" "l_linestatus" "l_shipdate" "l_commitdate" "l_receiptdate" "l_shipinstruct" "l_shipmode" "l_comment"
"1" "155190" "7706" "1" "17" "21168.23" "0.04" "0.02" "N" "O" "1996-03-13" "1996-02-12" "1996-03-22" "DELIVER IN PERSON" "TRUCK" "egular courts above the"
"1" "67310" "7311" "2" "36" "45983.16" "0.09" "0.06" "N" "O" "1996-04-12" "1996-02-28" "1996-04-20" "TAKE BACK RETURN" "MAIL" "ly final dependencies: slyly bold "
"1" "63700" "3701" "3" "8" "13309.6" "0.1" "0.02" "N" "O" "1996-01-29" "1996-03-05" "1996-01-31" "TAKE BACK RETURN" "REG AIR" "riously. regular, express dep"
"1" "2132" "4633" "4" "28" "28955.64" "0.09" "0.06" "N" "O" "1996-04-21" "1996-03-30" "1996-05-16" "NONE" "AIR" "lites. fluffily even de"
"1" "24027" "1534" "5" "24" "22824.48" "0.1" "0.04" "N" "O" "1996-03-30" "1996-03-14" "1996-04-01" "NONE" "FOB" " pending foxes. slyly re"
"1" "15635" "638" "6" "32" "49620.16" "0.07" "0.02" "N" "O" "1996-01-30" "1996-02-07" "1996-02-03" "DELIVER IN PERSON" "MAIL" "arefully slyly ex"
"2" "106170" "1191" "1" "38" "44694.46" "0.0" "0.05" "N" "O" "1997-01-28" "1997-01-14" "1997-02-02" "TAKE BACK RETURN" "RAIL" "ven requests. deposits breach a"
"3" "4297" "1798" "1" "45" "54058.05" "0.06" "0.0" "R" "F" "1994-02-02" "1994-01-04" "1994-02-23" "NONE" "AIR" "ongside of the furiously brave acco"
"3" "19036" "6540" "2" "49" "46796.47" "0.1" "0.0" "R" "F" "1993-11-09" "1993-12-20" "1993-11-24" "TAKE BACK RETURN" "RAIL" " unusual accounts. eve"
"3" "128449" "3474" "3" "27" "39890.88" "0.06" "0.07" "A" "F" "1994-01-16" "1993-11-22" "1994-01-23" "DELIVER IN PERSON" "SHIP" "nal foxes wake. "
'''


expected_results[all_types_source]["ascii"] = '''bool
tinyint
smallint
int
bigint
hugeint
uhugeint
utinyint
usmallint
uint
ubigint
bignum
date
time
timestamp
timestamp_s
timestamp_ms
timestamp_ns
time_tz
timestamp_tz
timestamp_tz_ns
float
double
dec_4_1
dec_9_4
dec_18_6
dec38_10
uuid
interval
varchar
blob
bit
small_enum
medium_enum
large_enum
int_array
double_array
date_array
timestamp_array
timestamptz_array
varchar_array
nested_int_array
struct
empty_struct
struct_of_arrays
array_of_structs
map
union
fixed_int_array
fixed_varchar_array
fixed_nested_int_array
fixed_nested_varchar_array
fixed_struct_array
struct_of_fixed_array
fixed_array_of_int_list
list_of_fixed_int_array
time_ns
geometry
tuple
geography
false
-128
-32768
-2147483648
-9223372036854775808
-170141183460469231731687303715884105728
0
0
0
0
0
-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368
5877642-06-25 (BC)
00:00:00
290309-12-22 (BC) 00:00:00
290309-12-22 (BC) 00:00:00
290309-12-22 (BC) 00:00:00
1677-09-22 00:00:00
00:00:00+15:59:59
290309-12-22 (BC) 00:00:00+00
1677-09-22 00:00:00+00
-3.4028235e+38
-1.7976931348623157e+308
-999.9
-99999.9999
-999999999999.999999
-9999999999999999999999999999.9999999999
00000000-0000-0000-0000-000000000000
00:00:00
🦆🦆🦆🦆🦆🦆
thisisalongblob\\x00withnullbytes
0010001001011100010101011010111
DUCK_DUCK_ENUM
enum_0
enum_0
[]
[]
[]
[]
[]
[]
[]
{'a': NULL, 'b': NULL}
{}
{'a': NULL, 'b': NULL}
[]
{}
Frank
[NULL, 2, 3]
[a, NULL, c]
[[NULL, 2, 3], NULL, [NULL, 2, 3]]
[[a, NULL, c], NULL, [a, NULL, c]]
[{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]
{'a': [NULL, 2, 3], 'b': [a, NULL, c]}
[[], [42, 999, NULL, NULL, -42], []]
[[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]
00:00:00
POINT EMPTY
(NULL, NULL)
POINT EMPTY
true
127
32767
2147483647
9223372036854775807
170141183460469231731687303715884105727
340282366920938463463374607431768211455
255
65535
4294967295
18446744073709551615
179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368
5881580-07-10
24:00:00
294247-01-10 04:00:54.775806
294247-01-10 04:00:54
294247-01-10 04:00:54.775
2262-04-11 23:47:16.854775806
24:00:00-15:59:59
294247-01-10 04:00:54.775806+00
2262-04-11 23:47:16.854775806+00
3.4028235e+38
1.7976931348623157e+308
999.9
99999.9999
999999999999.999999
9999999999999999999999999999.9999999999
ffffffff-ffff-ffff-ffff-ffffffffffff
83 years 3 months 999 days 00:16:39.999999
goo\\0se
\\x00\\x00\\x00a
10101
GOOSE
enum_299
enum_69999
[42, 999, NULL, NULL, -42]
[42.0, nan, inf, -inf, NULL, -42.0]
[1970-01-01, infinity, -infinity, NULL, 2022-05-12]
['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45']
['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00']
[🦆🦆🦆🦆🦆🦆, goose, NULL, '']
[[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]
{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}
{}
{'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']}
[{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL]
{key1=🦆🦆🦆🦆🦆🦆, key2=goose}
5
[4, 5, 6]
[d, e, f]
[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]
[[d, e, f], [a, NULL, c], [d, e, f]]
[{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}]
{'a': [4, 5, 6], 'b': [d, e, f]}
[[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]
[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]
24:00:00
GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)
(42, 🦆🦆🦆🦆🦆🦆)
LINESTRING (-180 -90, 180 90)
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
NULL
'''

expected_results[all_types_source]["box"] = '''┌───────┬─────────┬──────────┬─────────────┬──────────────────────┬──────────────────────────────────────────┬─────────────────────────────────────────┬──────────┬───────────┬────────────┬──────────────────────┬────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┬────────────────────┬──────────┬──────────────────────────────┬────────────────────────────┬────────────────────────────┬───────────────────────────────┬───────────────────┬─────────────────────────────────┬──────────────────────────────────┬────────────────┬──────────────────────────┬─────────┬─────────────┬──────────────────────┬──────────────────────────────────────────┬──────────────────────────────────────┬────────────────────────────────────────────┬──────────────┬──────────────────────────────────┬─────────────────────────────────┬────────────────┬─────────────┬────────────┬────────────────────────────┬─────────────────────────────────────┬─────────────────────────────────────────────────────┬───────────────────────────────────────────────────────────────────────────┬─────────────────────────────────────────────────────────────────────────────────┬─────────────────────────────────┬────────────────────────────────────────────────────────────────────────┬──────────────────────────────┬──────────────┬─────────────────────────────────────────────────────────────────────────┬──────────────────────────────────────────────────────────────┬─────────────────────────────────┬───────┬─────────────────┬─────────────────────┬──────────────────────────────────────┬──────────────────────────────────────┬──────────────────────────────────────────────────────────────────────────────────────┬────────────────────────────────────────┬──────────────────────────────────────────────────────────────┬─────────────────────────────────────────┬──────────┬───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┬────────────────────┬───────────────────────────────┐
│ bool  │ tinyint │ smallint │     int     │        bigint        │                 hugeint                  │                uhugeint                 │ utinyint │ usmallint │    uint    │       ubigint        │                                                                                                                                                         bignum                                                                                                                                                         │        date        │   time   │          timestamp           │        timestamp_s         │        timestamp_ms        │         timestamp_ns          │      time_tz      │          timestamp_tz           │         timestamp_tz_ns          │     float      │          double          │ dec_4_1 │   dec_9_4   │       dec_18_6       │                 dec38_10                 │                 uuid                 │                  interval                  │   varchar    │               blob               │               bit               │   small_enum   │ medium_enum │ large_enum │         int_array          │            double_array             │                     date_array                      │                              timestamp_array                              │                                timestamptz_array                                │          varchar_array          │                            nested_int_array                            │            struct            │ empty_struct │                            struct_of_arrays                             │                       array_of_structs                       │               map               │ union │ fixed_int_array │ fixed_varchar_array │        fixed_nested_int_array        │      fixed_nested_varchar_array      │                                  fixed_struct_array                                  │         struct_of_fixed_array          │                   fixed_array_of_int_list                    │         list_of_fixed_int_array         │ time_ns  │                                                                                                                                                                                                         geometry                                                                                                                                                                                                          │       tuple        │           geography           │
├───────┼─────────┼──────────┼─────────────┼──────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────────┼──────────┼───────────┼────────────┼──────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┼────────────────────┼──────────┼──────────────────────────────┼────────────────────────────┼────────────────────────────┼───────────────────────────────┼───────────────────┼─────────────────────────────────┼──────────────────────────────────┼────────────────┼──────────────────────────┼─────────┼─────────────┼──────────────────────┼──────────────────────────────────────────┼──────────────────────────────────────┼────────────────────────────────────────────┼──────────────┼──────────────────────────────────┼─────────────────────────────────┼────────────────┼─────────────┼────────────┼────────────────────────────┼─────────────────────────────────────┼─────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────┼────────────────────────────────────────────────────────────────────────┼──────────────────────────────┼──────────────┼─────────────────────────────────────────────────────────────────────────┼──────────────────────────────────────────────────────────────┼─────────────────────────────────┼───────┼─────────────────┼─────────────────────┼──────────────────────────────────────┼──────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────────────────────────────────────────────────┼─────────────────────────────────────────┼──────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┼────────────────────┼───────────────────────────────┤
│ false │ -128    │ -32768   │ -2147483648 │ -9223372036854775808 │ -170141183460469231731687303715884105728 │ 0                                       │ 0        │ 0         │ 0          │ 0                    │ -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368 │ 5877642-06-25 (BC) │ 00:00:00 │ 290309-12-22 (BC) 00:00:00   │ 290309-12-22 (BC) 00:00:00 │ 290309-12-22 (BC) 00:00:00 │ 1677-09-22 00:00:00           │ 00:00:00+15:59:59 │ 290309-12-22 (BC) 00:00:00+00   │ 1677-09-22 00:00:00+00           │ -3.4028235e+38 │ -1.7976931348623157e+308 │ -999.9  │ -99999.9999 │ -999999999999.999999 │ -9999999999999999999999999999.9999999999 │ 00000000-0000-0000-0000-000000000000 │ 00:00:00                                   │ 🦆🦆🦆🦆🦆🦆 │ thisisalongblob\\x00withnullbytes │ 0010001001011100010101011010111 │ DUCK_DUCK_ENUM │ enum_0      │ enum_0     │ []                         │ []                                  │ []                                                  │ []                                                                        │ []                                                                              │ []                              │ []                                                                     │ {'a': NULL, 'b': NULL}       │ {}           │ {'a': NULL, 'b': NULL}                                                  │ []                                                           │ {}                              │ Frank │ [NULL, 2, 3]    │ [a, NULL, c]        │ [[NULL, 2, 3], NULL, [NULL, 2, 3]]   │ [[a, NULL, c], NULL, [a, NULL, c]]   │ [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]       │ {'a': [NULL, 2, 3], 'b': [a, NULL, c]} │ [[], [42, 999, NULL, NULL, -42], []]                         │ [[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]] │ 00:00:00 │ POINT EMPTY                                                                                                                                                                                                                                                                                                                                                                                                               │ (NULL, NULL)       │ POINT EMPTY                   │
│ true  │ 127     │ 32767    │ 2147483647  │ 9223372036854775807  │ 170141183460469231731687303715884105727  │ 340282366920938463463374607431768211455 │ 255      │ 65535     │ 4294967295 │ 18446744073709551615 │ 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368  │ 5881580-07-10      │ 24:00:00 │ 294247-01-10 04:00:54.775806 │ 294247-01-10 04:00:54      │ 294247-01-10 04:00:54.775  │ 2262-04-11 23:47:16.854775806 │ 24:00:00-15:59:59 │ 294247-01-10 04:00:54.775806+00 │ 2262-04-11 23:47:16.854775806+00 │ 3.4028235e+38  │ 1.7976931348623157e+308  │ 999.9   │ 99999.9999  │ 999999999999.999999  │ 9999999999999999999999999999.9999999999  │ ffffffff-ffff-ffff-ffff-ffffffffffff │ 83 years 3 months 999 days 00:16:39.999999 │ goo\\0se      │ \\x00\\x00\\x00a                    │ 10101                           │ GOOSE          │ enum_299    │ enum_69999 │ [42, 999, NULL, NULL, -42] │ [42.0, nan, inf, -inf, NULL, -42.0] │ [1970-01-01, infinity, -infinity, NULL, 2022-05-12] │ ['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45'] │ ['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00'] │ [🦆🦆🦆🦆🦆🦆, goose, NULL, ''] │ [[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]] │ {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆} │ {}           │ {'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']} │ [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL] │ {key1=🦆🦆🦆🦆🦆🦆, key2=goose} │ 5     │ [4, 5, 6]       │ [d, e, f]           │ [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]] │ [[d, e, f], [a, NULL, c], [d, e, f]] │ [{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}] │ {'a': [4, 5, 6], 'b': [d, e, f]}       │ [[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]] │ [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]    │ 24:00:00 │ GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY) │ (42, 🦆🦆🦆🦆🦆🦆) │ LINESTRING (-180 -90, 180 90) │
│ NULL  │ NULL    │ NULL     │ NULL        │ NULL                 │ NULL                                     │ NULL                                    │ NULL     │ NULL      │ NULL       │ NULL                 │ NULL                                                                                                                                                                                                                                                                                                                   │ NULL               │ NULL     │ NULL                         │ NULL                       │ NULL                       │ NULL                          │ NULL              │ NULL                            │ NULL                             │ NULL           │ NULL                     │ NULL    │ NULL        │ NULL                 │ NULL                                     │ NULL                                 │ NULL                                       │ NULL         │ NULL                             │ NULL                            │ NULL           │ NULL        │ NULL       │ NULL                       │ NULL                                │ NULL                                                │ NULL                                                                      │ NULL                                                                            │ NULL                            │ NULL                                                                   │ NULL                         │ NULL         │ NULL                                                                    │ NULL                                                         │ NULL                            │ NULL  │ NULL            │ NULL                │ NULL                                 │ NULL                                 │ NULL                                                                                 │ NULL                                   │ NULL                                                         │ NULL                                    │ NULL     │ NULL                                                                                                                                                                                                                                                                                                                                                                                                                      │ NULL               │ NULL                          │
└───────┴─────────┴──────────┴─────────────┴──────────────────────┴──────────────────────────────────────────┴─────────────────────────────────────────┴──────────┴───────────┴────────────┴──────────────────────┴────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┴────────────────────┴──────────┴──────────────────────────────┴────────────────────────────┴────────────────────────────┴───────────────────────────────┴───────────────────┴─────────────────────────────────┴──────────────────────────────────┴────────────────┴──────────────────────────┴─────────┴─────────────┴──────────────────────┴──────────────────────────────────────────┴──────────────────────────────────────┴────────────────────────────────────────────┴──────────────┴──────────────────────────────────┴─────────────────────────────────┴────────────────┴─────────────┴────────────┴────────────────────────────┴─────────────────────────────────────┴─────────────────────────────────────────────────────┴───────────────────────────────────────────────────────────────────────────┴─────────────────────────────────────────────────────────────────────────────────┴─────────────────────────────────┴────────────────────────────────────────────────────────────────────────┴──────────────────────────────┴──────────────┴─────────────────────────────────────────────────────────────────────────┴──────────────────────────────────────────────────────────────┴─────────────────────────────────┴───────┴─────────────────┴─────────────────────┴──────────────────────────────────────┴──────────────────────────────────────┴──────────────────────────────────────────────────────────────────────────────────────┴────────────────────────────────────────┴──────────────────────────────────────────────────────────────┴─────────────────────────────────────────┴──────────┴───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┴────────────────────┴───────────────────────────────┘
'''

expected_results[all_types_source]["column"] = '''bool   tinyint  smallint  int          bigint                hugeint                                   uhugeint                                 utinyint  usmallint  uint        ubigint               bignum                                                                                                                                                                                                                                                                                                                  date                time      timestamp                     timestamp_s                 timestamp_ms                timestamp_ns                   time_tz            timestamp_tz                     timestamp_tz_ns                   float           double                    dec_4_1  dec_9_4      dec_18_6              dec38_10                                  uuid                                  interval                                    varchar       blob                              bit                              small_enum      medium_enum  large_enum  int_array                   double_array                         date_array                                           timestamp_array                                                            timestamptz_array                                                                varchar_array                    nested_int_array                                                        struct                        empty_struct  struct_of_arrays                                                         array_of_structs                                              map                              union  fixed_int_array  fixed_varchar_array  fixed_nested_int_array                fixed_nested_varchar_array            fixed_struct_array                                                                    struct_of_fixed_array                   fixed_array_of_int_list                                       list_of_fixed_int_array                  time_ns   geometry                                                                                                                                                                                                                                                                                                                                                                                                                   tuple               geography                    
-----  -------  --------  -----------  --------------------  ----------------------------------------  ---------------------------------------  --------  ---------  ----------  --------------------  ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------  ------------------  --------  ----------------------------  --------------------------  --------------------------  -----------------------------  -----------------  -------------------------------  --------------------------------  --------------  ------------------------  -------  -----------  --------------------  ----------------------------------------  ------------------------------------  ------------------------------------------  ------------  --------------------------------  -------------------------------  --------------  -----------  ----------  --------------------------  -----------------------------------  ---------------------------------------------------  -------------------------------------------------------------------------  -------------------------------------------------------------------------------  -------------------------------  ----------------------------------------------------------------------  ----------------------------  ------------  -----------------------------------------------------------------------  ------------------------------------------------------------  -------------------------------  -----  ---------------  -------------------  ------------------------------------  ------------------------------------  ------------------------------------------------------------------------------------  --------------------------------------  ------------------------------------------------------------  ---------------------------------------  --------  -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------  ------------------  -----------------------------
false  -128     -32768    -2147483648  -9223372036854775808  -170141183460469231731687303715884105728  0                                        0         0          0           0                     -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368  5877642-06-25 (BC)  00:00:00  290309-12-22 (BC) 00:00:00    290309-12-22 (BC) 00:00:00  290309-12-22 (BC) 00:00:00  1677-09-22 00:00:00            00:00:00+15:59:59  290309-12-22 (BC) 00:00:00+00    1677-09-22 00:00:00+00            -3.4028235e+38  -1.7976931348623157e+308  -999.9   -99999.9999  -999999999999.999999  -9999999999999999999999999999.9999999999  00000000-0000-0000-0000-000000000000  00:00:00                                    🦆🦆🦆🦆🦆🦆  thisisalongblob\\x00withnullbytes  0010001001011100010101011010111  DUCK_DUCK_ENUM  enum_0       enum_0      []                          []                                   []                                                   []                                                                         []                                                                               []                               []                                                                      {'a': NULL, 'b': NULL}        {}            {'a': NULL, 'b': NULL}                                                   []                                                            {}                               Frank  [NULL, 2, 3]     [a, NULL, c]         [[NULL, 2, 3], NULL, [NULL, 2, 3]]    [[a, NULL, c], NULL, [a, NULL, c]]    [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]        {'a': [NULL, 2, 3], 'b': [a, NULL, c]}  [[], [42, 999, NULL, NULL, -42], []]                          [[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]  00:00:00  POINT EMPTY                                                                                                                                                                                                                                                                                                                                                                                                                (NULL, NULL)        POINT EMPTY                  
true   127      32767     2147483647   9223372036854775807   170141183460469231731687303715884105727   340282366920938463463374607431768211455  255       65535      4294967295  18446744073709551615  179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368   5881580-07-10       24:00:00  294247-01-10 04:00:54.775806  294247-01-10 04:00:54       294247-01-10 04:00:54.775   2262-04-11 23:47:16.854775806  24:00:00-15:59:59  294247-01-10 04:00:54.775806+00  2262-04-11 23:47:16.854775806+00  3.4028235e+38   1.7976931348623157e+308   999.9    99999.9999   999999999999.999999   9999999999999999999999999999.9999999999   ffffffff-ffff-ffff-ffff-ffffffffffff  83 years 3 months 999 days 00:16:39.999999  goo\\0se       \\x00\\x00\\x00a                     10101                            GOOSE           enum_299     enum_69999  [42, 999, NULL, NULL, -42]  [42.0, nan, inf, -inf, NULL, -42.0]  [1970-01-01, infinity, -infinity, NULL, 2022-05-12]  ['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45']  ['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00']  [🦆🦆🦆🦆🦆🦆, goose, NULL, '']  [[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]  {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}  {}            {'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']}  [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL]  {key1=🦆🦆🦆🦆🦆🦆, key2=goose}  5      [4, 5, 6]        [d, e, f]            [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]  [[d, e, f], [a, NULL, c], [d, e, f]]  [{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}]  {'a': [4, 5, 6], 'b': [d, e, f]}        [[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]  [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]     24:00:00  GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)  (42, 🦆🦆🦆🦆🦆🦆)  LINESTRING (-180 -90, 180 90)
NULL   NULL     NULL      NULL         NULL                  NULL                                      NULL                                     NULL      NULL       NULL        NULL                  NULL                                                                                                                                                                                                                                                                                                                    NULL                NULL      NULL                          NULL                        NULL                        NULL                           NULL               NULL                             NULL                              NULL            NULL                      NULL     NULL         NULL                  NULL                                      NULL                                  NULL                                        NULL          NULL                              NULL                             NULL            NULL         NULL        NULL                        NULL                                 NULL                                                 NULL                                                                       NULL                                                                             NULL                             NULL                                                                    NULL                          NULL          NULL                                                                     NULL                                                          NULL                             NULL   NULL             NULL                 NULL                                  NULL                                  NULL                                                                                  NULL                                    NULL                                                          NULL                                     NULL      NULL                                                                                                                                                                                                                                                                                                                                                                                                                       NULL                NULL                         
'''

expected_results[all_types_source]["csv"] = '''bool,tinyint,smallint,int,bigint,hugeint,uhugeint,utinyint,usmallint,uint,ubigint,bignum,date,time,timestamp,timestamp_s,timestamp_ms,timestamp_ns,time_tz,timestamp_tz,timestamp_tz_ns,float,double,dec_4_1,dec_9_4,dec_18_6,dec38_10,uuid,interval,varchar,blob,bit,small_enum,medium_enum,large_enum,int_array,double_array,date_array,timestamp_array,timestamptz_array,varchar_array,nested_int_array,struct,empty_struct,struct_of_arrays,array_of_structs,map,union,fixed_int_array,fixed_varchar_array,fixed_nested_int_array,fixed_nested_varchar_array,fixed_struct_array,struct_of_fixed_array,fixed_array_of_int_list,list_of_fixed_int_array,time_ns,geometry,tuple,geography
false,-128,-32768,-2147483648,-9223372036854775808,-170141183460469231731687303715884105728,0,0,0,0,0,-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368,5877642-06-25 (BC),00:00:00,290309-12-22 (BC) 00:00:00,290309-12-22 (BC) 00:00:00,290309-12-22 (BC) 00:00:00,1677-09-22 00:00:00,00:00:00+15:59:59,290309-12-22 (BC) 00:00:00+00,1677-09-22 00:00:00+00,-3.4028235e+38,-1.7976931348623157e+308,-999.9,-99999.9999,-999999999999.999999,-9999999999999999999999999999.9999999999,00000000-0000-0000-0000-000000000000,00:00:00,"🦆🦆🦆🦆🦆🦆",thisisalongblob\\x00withnullbytes,0010001001011100010101011010111,DUCK_DUCK_ENUM,enum_0,enum_0,[],[],[],[],[],[],[],"{'a': NULL, 'b': NULL}",{},"{'a': NULL, 'b': NULL}",[],{},Frank,"[NULL, 2, 3]","[a, NULL, c]","[[NULL, 2, 3], NULL, [NULL, 2, 3]]","[[a, NULL, c], NULL, [a, NULL, c]]","[{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]","{'a': [NULL, 2, 3], 'b': [a, NULL, c]}","[[], [42, 999, NULL, NULL, -42], []]","[[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]",00:00:00,POINT EMPTY,"(NULL, NULL)",POINT EMPTY
true,127,32767,2147483647,9223372036854775807,170141183460469231731687303715884105727,340282366920938463463374607431768211455,255,65535,4294967295,18446744073709551615,179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368,5881580-07-10,24:00:00,294247-01-10 04:00:54.775806,294247-01-10 04:00:54,294247-01-10 04:00:54.775,2262-04-11 23:47:16.854775806,24:00:00-15:59:59,294247-01-10 04:00:54.775806+00,2262-04-11 23:47:16.854775806+00,3.4028235e+38,1.7976931348623157e+308,999.9,99999.9999,999999999999.999999,9999999999999999999999999999.9999999999,ffffffff-ffff-ffff-ffff-ffffffffffff,83 years 3 months 999 days 00:16:39.999999,goo\\0se,\\x00\\x00\\x00a,10101,GOOSE,enum_299,enum_69999,"[42, 999, NULL, NULL, -42]","[42.0, nan, inf, -inf, NULL, -42.0]","[1970-01-01, infinity, -infinity, NULL, 2022-05-12]","['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45']","['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00']","[🦆🦆🦆🦆🦆🦆, goose, NULL, '']","[[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]","{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}",{},"{'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']}","[{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL]","{key1=🦆🦆🦆🦆🦆🦆, key2=goose}",5,"[4, 5, 6]","[d, e, f]","[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]","[[d, e, f], [a, NULL, c], [d, e, f]]","[{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}]","{'a': [4, 5, 6], 'b': [d, e, f]}","[[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]","[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]",24:00:00,"GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)","(42, 🦆🦆🦆🦆🦆🦆)","LINESTRING (-180 -90, 180 90)"
NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL
'''

expected_results[all_types_source]["duckbox"] = '''┌─────────┬─────────┬──────────┬─────────────┬──────────────────────┬──────────────────────────────────────────┬─────────────────────────────────────────┬──────────┬───────────┬────────────┬──────────────────────┬────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┬────────────────────┬──────────┬──────────────────────────────┬────────────────────────────┬────────────────────────────┬───────────────────────────────┬─────────────────────┬─────────────────────────────────┬──────────────────────────────────┬────────────────┬──────────────────────────┬──────────────┬──────────────┬──────────────────────┬──────────────────────────────────────────┬──────────────────────────────────────┬────────────────────────────────────────────┬──────────────┬──────────────────────────────────┬─────────────────────────────────┬─────────────────────────────────┬────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┬──────────────────────────────┬────────────────────────────┬─────────────────────────────────────┬─────────────────────────────────────────────────────┬───────────────────────────────────────────────────────────────────────────┬─────────────────────────────────────────────────────────────────────────────────┬─────────────────────────────────┬────────────────────────────────────────────────────────────────────────┬──────────────────────────────┬──────────────┬─────────────────────────────────────────────────────────────────────────┬──────────────────────────────────────────────────────────────┬─────────────────────────────────┬─────────────────────────────────────┬─────────────────┬─────────────────────┬──────────────────────────────────────┬──────────────────────────────────────┬──────────────────────────────────────────────────────────────────────────────────────┬────────────────────────────────────────┬──────────────────────────────────────────────────────────────┬─────────────────────────────────────────┬──────────┬───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┬─────────────────────────┬───────────────────────────────┐
│  bool   │ tinyint │ smallint │     int     │        bigint        │                 hugeint                  │                uhugeint                 │ utinyint │ usmallint │    uint    │       ubigint        │                                                                                                                                                         bignum                                                                                                                                                         │        date        │   time   │          timestamp           │        timestamp_s         │        timestamp_ms        │         timestamp_ns          │       time_tz       │          timestamp_tz           │         timestamp_tz_ns          │     float      │          double          │   dec_4_1    │   dec_9_4    │       dec_18_6       │                 dec38_10                 │                 uuid                 │                  interval                  │   varchar    │               blob               │               bit               │           small_enum            │                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                              medium_enum                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               │          large_enum          │         int_array          │            double_array             │                     date_array                      │                              timestamp_array                              │                                timestamptz_array                                │          varchar_array          │                            nested_int_array                            │            struct            │ empty_struct │                            struct_of_arrays                             │                       array_of_structs                       │               map               │                union                │ fixed_int_array │ fixed_varchar_array │        fixed_nested_int_array        │      fixed_nested_varchar_array      │                                  fixed_struct_array                                  │         struct_of_fixed_array          │                   fixed_array_of_int_list                    │         list_of_fixed_int_array         │ time_ns  │                                                                                                                                                                                                         geometry                                                                                                                                                                                                          │          tuple          │           geography           │
│ boolean │  int8   │  int16   │    int32    │        int64         │                  int128                  │                 uint128                 │  uint8   │  uint16   │   uint32   │        uint64        │                                                                                                                                                         bignum                                                                                                                                                         │        date        │   time   │          timestamp           │        timestamp_s         │        timestamp_ms        │         timestamp_ns          │ time with time zone │    timestamp with time zone     │          timestamptz_ns          │     float      │          double          │ decimal(4,1) │ decimal(9,4) │    decimal(18,6)     │              decimal(38,10)              │                 uuid                 │                  interval                  │   varchar    │               blob               │               bit               │ enum('DUCK_DUCK_ENUM', 'GOOSE') │ enum('enum_0', 'enum_1', 'enum_2', 'enum_3', 'enum_4', 'enum_5', 'enum_6', 'enum_7', 'enum_8', 'enum_9', 'enum_10', 'enum_11', 'enum_12', 'enum_13', 'enum_14', 'enum_15', 'enum_16', 'enum_17', 'enum_18', 'enum_19', 'enum_20', 'enum_21', 'enum_22', 'enum_23', 'enum_24', 'enum_25', 'enum_26', 'enum_27', 'enum_28', 'enum_29', 'enum_30', 'enum_31', 'enum_32', 'enum_33', 'enum_34', 'enum_35', 'enum_36', 'enum_37', 'enum_38', 'enum_39', 'enum_40', 'enum_41', 'enum_42', 'enum_43', 'enum_44', 'enum_45', 'enum_46', 'enum_47', 'enum_48', 'enum_49', 'enum_50', 'enum_51', 'enum_52', 'enum_53', 'enum_54', 'enum_55', 'enum_56', 'enum_57', 'enum_58', 'enum_59', 'enum_60', 'enum_61', 'enum_62', 'enum_63', 'enum_64', 'enum_65', 'enum_66', 'enum_67', 'enum_68', 'enum_69', 'enum_70', 'enum_71', 'enum_72', 'enum_73', 'enum_74', 'enum_75', 'enum_76', 'enum_77', 'enum_78', 'enum_79', 'enum_80', 'enum_81', 'enum_82', 'enum_83', 'enum_84', 'enum_85', 'enum_86', 'enum_87', 'enum_88', 'enum_89', 'enum_90', 'enum_91', 'enum_92', 'enum_93', 'enum_94', 'enum_95', 'enum_96', 'enum_97', 'enum_98', 'enum_99', 'enum_100', 'enum_101', 'enum_102', 'enum_103', 'enum_104', 'enum_105', 'enum_106', 'enum_107', 'enum_108', 'enum_109', 'enum_110', 'enum_111', 'enum_112', 'enum_113', 'enum_114', 'enum_115', 'enum_116', 'enum_117', 'enum_118', 'enum_119', 'enum_120', 'enum_121', 'enum_122', 'enum_123', 'enum_124', 'enum_125', 'enum_126', 'enum_127', 'enum_128', 'enum_129', 'enum_130', 'enum_131', 'enum_132', 'enum_133', 'enum_134', 'enum_135', 'enum_136', 'enum_137', 'enum_138', 'enum_139', 'enum_140', 'enum_141', 'enum_142', 'enum_143', 'enum_144', 'enum_145', 'enum_146', 'enum_147', 'enum_148', 'enum_149', 'enum_150', 'enum_151', 'enum_152', 'enum_153', 'enum_154', 'enum_155', 'enum_156', 'enum_157', 'enum_158', 'enum_159', 'enum_160', 'enum_161', 'enum_162', 'enum_163', 'enum_164', 'enum_165', 'enum_166', 'enum_167', 'enum_168', 'enum_169', 'enum_170', 'enum_171', 'enum_172', 'enum_173', 'enum_174', 'enum_175', 'enum_176', 'enum_177', 'enum_178', 'enum_179', 'enum_180', 'enum_181', 'enum_182', 'enum_183', 'enum_184', 'enum_185', 'enum_186', 'enum_187', 'enum_188', 'enum_189', 'enum_190', 'enum_191', 'enum_192', 'enum_193', 'enum_194', 'enum_195', 'enum_196', 'enum_197', 'enum_198', 'enum_199', 'enum_200', 'enum_201', 'enum_202', 'enum_203', 'enum_204', 'enum_205', 'enum_206', 'enum_207', 'enum_208', 'enum_209', 'enum_210', 'enum_211', 'enum_212', 'enum_213', 'enum_214', 'enum_215', 'enum_216', 'enum_217', 'enum_218', 'enum_219', 'enum_220', 'enum_221', 'enum_222', 'enum_223', 'enum_224', 'enum_225', 'enum_226', 'enum_227', 'enum_228', 'enum_229', 'enum_230', 'enum_231', 'enum_232', 'enum_233', 'enum_234', 'enum_235', 'enum_236', 'enum_237', 'enum_238', 'enum_239', 'enum_240', 'enum_241', 'enum_242', 'enum_243', 'enum_244', 'enum_245', 'enum_246', 'enum_247', 'enum_248', 'enum_249', 'enum_250', 'enum_251', 'enum_252', 'enum_253', 'enum_254', 'enum_255', 'enum_256', 'enum_257', 'enum_258', 'enum_259', 'enum_260', 'enum_261', 'enum_262', 'enum_263', 'enum_264', 'enum_265', 'enum_266', 'enum_267', 'enum_268', 'enum_269', 'enum_270', 'enum_271', 'enum_272', 'enum_273', 'enum_274', 'enum_275', 'enum_276', 'enum_277', 'enum_278', 'enum_279', 'enum_280', 'enum_281', 'enum_282', 'enum_283', 'enum_284', 'enum_285', 'enum_286', 'enum_287', 'enum_288', 'enum_289', 'enum_290', 'enum_291', 'enum_292', 'enum_293', 'enum_294', 'enum_295', 'enum_296', 'enum_297', 'enum_298', 'enum_299') │ enum('enum_0', 'enum_69999') │          int32[]           │              double[]               │                       date[]                        │                                timestamp[]                                │                           timestamp with time zone[]                            │            varchar[]            │                               int32[][]                                │ struct(a integer, b varchar) │    struct    │                    struct(a integer[], b varchar[])                     │                struct(a integer, b varchar)[]                │      map(varchar, varchar)      │ union("name" varchar, age smallint) │   integer[3]    │     varchar[3]      │            integer[3][3]             │            varchar[3][3]             │                           struct(a integer, b varchar)[3]                            │   struct(a integer[3], b varchar[3])   │                         integer[][3]                         │              integer[3][]               │ time_ns  │                                                                                                                                                                                                         geometry                                                                                                                                                                                                          │ tuple(integer, varchar) │           geography           │
├─────────┼─────────┼──────────┼─────────────┼──────────────────────┼──────────────────────────────────────────┼─────────────────────────────────────────┼──────────┼───────────┼────────────┼──────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┼────────────────────┼──────────┼──────────────────────────────┼────────────────────────────┼────────────────────────────┼───────────────────────────────┼─────────────────────┼─────────────────────────────────┼──────────────────────────────────┼────────────────┼──────────────────────────┼──────────────┼──────────────┼──────────────────────┼──────────────────────────────────────────┼──────────────────────────────────────┼────────────────────────────────────────────┼──────────────┼──────────────────────────────────┼─────────────────────────────────┼─────────────────────────────────┼────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┼──────────────────────────────┼────────────────────────────┼─────────────────────────────────────┼─────────────────────────────────────────────────────┼───────────────────────────────────────────────────────────────────────────┼─────────────────────────────────────────────────────────────────────────────────┼─────────────────────────────────┼────────────────────────────────────────────────────────────────────────┼──────────────────────────────┼──────────────┼─────────────────────────────────────────────────────────────────────────┼──────────────────────────────────────────────────────────────┼─────────────────────────────────┼─────────────────────────────────────┼─────────────────┼─────────────────────┼──────────────────────────────────────┼──────────────────────────────────────┼──────────────────────────────────────────────────────────────────────────────────────┼────────────────────────────────────────┼──────────────────────────────────────────────────────────────┼─────────────────────────────────────────┼──────────┼───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┼─────────────────────────┼───────────────────────────────┤
│ false   │    -128 │   -32768 │ -2147483648 │ -9223372036854775808 │ -170141183460469231731687303715884105728 │                                       0 │        0 │         0 │          0 │                    0 │ -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368 │ 5877642-06-25 (BC) │ 00:00:00 │ 290309-12-22 (BC) 00:00:00   │ 290309-12-22 (BC) 00:00:00 │ 290309-12-22 (BC) 00:00:00 │ 1677-09-22 00:00:00           │ 00:00:00+15:59:59   │ 290309-12-22 (BC) 00:00:00+00   │ 1677-09-22 00:00:00+00           │ -3.4028235e+38 │ -1.7976931348623157e+308 │       -999.9 │  -99999.9999 │ -999999999999.999999 │ -9999999999999999999999999999.9999999999 │ 00000000-0000-0000-0000-000000000000 │ 00:00:00                                   │ 🦆🦆🦆🦆🦆🦆 │ thisisalongblob\\x00withnullbytes │ 0010001001011100010101011010111 │ DUCK_DUCK_ENUM                  │ enum_0                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                 │ enum_0                       │ []                         │ []                                  │ []                                                  │ []                                                                        │ []                                                                              │ []                              │ []                                                                     │ {'a': NULL, 'b': NULL}       │ {}           │ {'a': NULL, 'b': NULL}                                                  │ []                                                           │ {}                              │ Frank                               │ [NULL, 2, 3]    │ [a, NULL, c]        │ [[NULL, 2, 3], NULL, [NULL, 2, 3]]   │ [[a, NULL, c], NULL, [a, NULL, c]]   │ [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]       │ {'a': [NULL, 2, 3], 'b': [a, NULL, c]} │ [[], [42, 999, NULL, NULL, -42], []]                         │ [[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]] │ 00:00:00 │ POINT EMPTY                                                                                                                                                                                                                                                                                                                                                                                                               │ (NULL, NULL)            │ POINT EMPTY                   │
│ true    │     127 │    32767 │  2147483647 │  9223372036854775807 │  170141183460469231731687303715884105727 │ 340282366920938463463374607431768211455 │      255 │     65535 │ 4294967295 │ 18446744073709551615 │ 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368  │ 5881580-07-10      │ 24:00:00 │ 294247-01-10 04:00:54.775806 │ 294247-01-10 04:00:54      │ 294247-01-10 04:00:54.775  │ 2262-04-11 23:47:16.854775806 │ 24:00:00-15:59:59   │ 294247-01-10 04:00:54.775806+00 │ 2262-04-11 23:47:16.854775806+00 │  3.4028235e+38 │  1.7976931348623157e+308 │        999.9 │   99999.9999 │  999999999999.999999 │  9999999999999999999999999999.9999999999 │ ffffffff-ffff-ffff-ffff-ffffffffffff │ 83 years 3 months 999 days 00:16:39.999999 │ goo\\0se      │ \\x00\\x00\\x00a                    │ 10101                           │ GOOSE                           │ enum_299                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                               │ enum_69999                   │ [42, 999, NULL, NULL, -42] │ [42.0, nan, inf, -inf, NULL, -42.0] │ [1970-01-01, infinity, -infinity, NULL, 2022-05-12] │ ['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45'] │ ['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00'] │ [🦆🦆🦆🦆🦆🦆, goose, NULL, ''] │ [[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]] │ {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆} │ {}           │ {'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']} │ [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL] │ {key1=🦆🦆🦆🦆🦆🦆, key2=goose} │ 5                                   │ [4, 5, 6]       │ [d, e, f]           │ [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]] │ [[d, e, f], [a, NULL, c], [d, e, f]] │ [{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}] │ {'a': [4, 5, 6], 'b': [d, e, f]}       │ [[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]] │ [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]    │ 24:00:00 │ GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY) │ (42, 🦆🦆🦆🦆🦆🦆)      │ LINESTRING (-180 -90, 180 90) │
│ NULL    │    NULL │     NULL │        NULL │                 NULL │                                     NULL │                                    NULL │     NULL │      NULL │       NULL │                 NULL │ NULL                                                                                                                                                                                                                                                                                                                   │ NULL               │ NULL     │ NULL                         │ NULL                       │ NULL                       │ NULL                          │ NULL                │ NULL                            │ NULL                             │           NULL │                     NULL │         NULL │         NULL │                 NULL │                                     NULL │ NULL                                 │ NULL                                       │ NULL         │ NULL                             │ NULL                            │ NULL                            │ NULL                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   │ NULL                         │ NULL                       │ NULL                                │ NULL                                                │ NULL                                                                      │ NULL                                                                            │ NULL                            │ NULL                                                                   │ NULL                         │ NULL         │ NULL                                                                    │ NULL                                                         │ NULL                            │ NULL                                │ NULL            │ NULL                │ NULL                                 │ NULL                                 │ NULL                                                                                 │ NULL                                   │ NULL                                                         │ NULL                                    │ NULL     │ NULL                                                                                                                                                                                                                                                                                                                                                                                                                      │ NULL                    │ NULL                          │
└─────────┴─────────┴──────────┴─────────────┴──────────────────────┴──────────────────────────────────────────┴─────────────────────────────────────────┴──────────┴───────────┴────────────┴──────────────────────┴────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┴────────────────────┴──────────┴──────────────────────────────┴────────────────────────────┴────────────────────────────┴───────────────────────────────┴─────────────────────┴─────────────────────────────────┴──────────────────────────────────┴────────────────┴──────────────────────────┴──────────────┴──────────────┴──────────────────────┴──────────────────────────────────────────┴──────────────────────────────────────┴────────────────────────────────────────────┴──────────────┴──────────────────────────────────┴─────────────────────────────────┴─────────────────────────────────┴────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┴──────────────────────────────┴────────────────────────────┴─────────────────────────────────────┴─────────────────────────────────────────────────────┴───────────────────────────────────────────────────────────────────────────┴─────────────────────────────────────────────────────────────────────────────────┴─────────────────────────────────┴────────────────────────────────────────────────────────────────────────┴──────────────────────────────┴──────────────┴─────────────────────────────────────────────────────────────────────────┴──────────────────────────────────────────────────────────────┴─────────────────────────────────┴─────────────────────────────────────┴─────────────────┴─────────────────────┴──────────────────────────────────────┴──────────────────────────────────────┴──────────────────────────────────────────────────────────────────────────────────────┴────────────────────────────────────────┴──────────────────────────────────────────────────────────────┴─────────────────────────────────────────┴──────────┴───────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┴─────────────────────────┴───────────────────────────────┘
'''

expected_results[all_types_source]["html"] = '''<tr><th>bool</th>
<th>tinyint</th>
<th>smallint</th>
<th>int</th>
<th>bigint</th>
<th>hugeint</th>
<th>uhugeint</th>
<th>utinyint</th>
<th>usmallint</th>
<th>uint</th>
<th>ubigint</th>
<th>bignum</th>
<th>date</th>
<th>time</th>
<th>timestamp</th>
<th>timestamp_s</th>
<th>timestamp_ms</th>
<th>timestamp_ns</th>
<th>time_tz</th>
<th>timestamp_tz</th>
<th>timestamp_tz_ns</th>
<th>float</th>
<th>double</th>
<th>dec_4_1</th>
<th>dec_9_4</th>
<th>dec_18_6</th>
<th>dec38_10</th>
<th>uuid</th>
<th>interval</th>
<th>varchar</th>
<th>blob</th>
<th>bit</th>
<th>small_enum</th>
<th>medium_enum</th>
<th>large_enum</th>
<th>int_array</th>
<th>double_array</th>
<th>date_array</th>
<th>timestamp_array</th>
<th>timestamptz_array</th>
<th>varchar_array</th>
<th>nested_int_array</th>
<th>struct</th>
<th>empty_struct</th>
<th>struct_of_arrays</th>
<th>array_of_structs</th>
<th>map</th>
<th>union</th>
<th>fixed_int_array</th>
<th>fixed_varchar_array</th>
<th>fixed_nested_int_array</th>
<th>fixed_nested_varchar_array</th>
<th>fixed_struct_array</th>
<th>struct_of_fixed_array</th>
<th>fixed_array_of_int_list</th>
<th>list_of_fixed_int_array</th>
<th>time_ns</th>
<th>geometry</th>
<th>tuple</th>
<th>geography</th>
</tr>
<tr><td>false</td>
<td>-128</td>
<td>-32768</td>
<td>-2147483648</td>
<td>-9223372036854775808</td>
<td>-170141183460469231731687303715884105728</td>
<td>0</td>
<td>0</td>
<td>0</td>
<td>0</td>
<td>0</td>
<td>-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368</td>
<td>5877642-06-25 (BC)</td>
<td>00:00:00</td>
<td>290309-12-22 (BC) 00:00:00</td>
<td>290309-12-22 (BC) 00:00:00</td>
<td>290309-12-22 (BC) 00:00:00</td>
<td>1677-09-22 00:00:00</td>
<td>00:00:00+15:59:59</td>
<td>290309-12-22 (BC) 00:00:00+00</td>
<td>1677-09-22 00:00:00+00</td>
<td>-3.4028235e+38</td>
<td>-1.7976931348623157e+308</td>
<td>-999.9</td>
<td>-99999.9999</td>
<td>-999999999999.999999</td>
<td>-9999999999999999999999999999.9999999999</td>
<td>00000000-0000-0000-0000-000000000000</td>
<td>00:00:00</td>
<td>🦆🦆🦆🦆🦆🦆</td>
<td>thisisalongblob\\x00withnullbytes</td>
<td>0010001001011100010101011010111</td>
<td>DUCK_DUCK_ENUM</td>
<td>enum_0</td>
<td>enum_0</td>
<td>[]</td>
<td>[]</td>
<td>[]</td>
<td>[]</td>
<td>[]</td>
<td>[]</td>
<td>[]</td>
<td>{&#39;a&#39;: NULL, &#39;b&#39;: NULL}</td>
<td>{}</td>
<td>{&#39;a&#39;: NULL, &#39;b&#39;: NULL}</td>
<td>[]</td>
<td>{}</td>
<td>Frank</td>
<td>[NULL, 2, 3]</td>
<td>[a, NULL, c]</td>
<td>[[NULL, 2, 3], NULL, [NULL, 2, 3]]</td>
<td>[[a, NULL, c], NULL, [a, NULL, c]]</td>
<td>[{&#39;a&#39;: NULL, &#39;b&#39;: NULL}, {&#39;a&#39;: 42, &#39;b&#39;: 🦆🦆🦆🦆🦆🦆}, {&#39;a&#39;: NULL, &#39;b&#39;: NULL}]</td>
<td>{&#39;a&#39;: [NULL, 2, 3], &#39;b&#39;: [a, NULL, c]}</td>
<td>[[], [42, 999, NULL, NULL, -42], []]</td>
<td>[[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]</td>
<td>00:00:00</td>
<td>POINT EMPTY</td>
<td>(NULL, NULL)</td>
<td>POINT EMPTY</td>
</tr>
<tr><td>true</td>
<td>127</td>
<td>32767</td>
<td>2147483647</td>
<td>9223372036854775807</td>
<td>170141183460469231731687303715884105727</td>
<td>340282366920938463463374607431768211455</td>
<td>255</td>
<td>65535</td>
<td>4294967295</td>
<td>18446744073709551615</td>
<td>179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368</td>
<td>5881580-07-10</td>
<td>24:00:00</td>
<td>294247-01-10 04:00:54.775806</td>
<td>294247-01-10 04:00:54</td>
<td>294247-01-10 04:00:54.775</td>
<td>2262-04-11 23:47:16.854775806</td>
<td>24:00:00-15:59:59</td>
<td>294247-01-10 04:00:54.775806+00</td>
<td>2262-04-11 23:47:16.854775806+00</td>
<td>3.4028235e+38</td>
<td>1.7976931348623157e+308</td>
<td>999.9</td>
<td>99999.9999</td>
<td>999999999999.999999</td>
<td>9999999999999999999999999999.9999999999</td>
<td>ffffffff-ffff-ffff-ffff-ffffffffffff</td>
<td>83 years 3 months 999 days 00:16:39.999999</td>
<td>goo\\0se</td>
<td>\\x00\\x00\\x00a</td>
<td>10101</td>
<td>GOOSE</td>
<td>enum_299</td>
<td>enum_69999</td>
<td>[42, 999, NULL, NULL, -42]</td>
<td>[42.0, nan, inf, -inf, NULL, -42.0]</td>
<td>[1970-01-01, infinity, -infinity, NULL, 2022-05-12]</td>
<td>[&#39;1970-01-01 00:00:00&#39;, infinity, -infinity, NULL, &#39;2022-05-12 16:23:45&#39;]</td>
<td>[&#39;1970-01-01 00:00:00+00&#39;, infinity, -infinity, NULL, &#39;2022-05-12 23:23:45+00&#39;]</td>
<td>[🦆🦆🦆🦆🦆🦆, goose, NULL, &#39;&#39;]</td>
<td>[[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]</td>
<td>{&#39;a&#39;: 42, &#39;b&#39;: 🦆🦆🦆🦆🦆🦆}</td>
<td>{}</td>
<td>{&#39;a&#39;: [42, 999, NULL, NULL, -42], &#39;b&#39;: [🦆🦆🦆🦆🦆🦆, goose, NULL, &#39;&#39;]}</td>
<td>[{&#39;a&#39;: NULL, &#39;b&#39;: NULL}, {&#39;a&#39;: 42, &#39;b&#39;: 🦆🦆🦆🦆🦆🦆}, NULL]</td>
<td>{key1=🦆🦆🦆🦆🦆🦆, key2=goose}</td>
<td>5</td>
<td>[4, 5, 6]</td>
<td>[d, e, f]</td>
<td>[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]</td>
<td>[[d, e, f], [a, NULL, c], [d, e, f]]</td>
<td>[{&#39;a&#39;: 42, &#39;b&#39;: 🦆🦆🦆🦆🦆🦆}, {&#39;a&#39;: NULL, &#39;b&#39;: NULL}, {&#39;a&#39;: 42, &#39;b&#39;: 🦆🦆🦆🦆🦆🦆}]</td>
<td>{&#39;a&#39;: [4, 5, 6], &#39;b&#39;: [d, e, f]}</td>
<td>[[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]</td>
<td>[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]</td>
<td>24:00:00</td>
<td>GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)</td>
<td>(42, 🦆🦆🦆🦆🦆🦆)</td>
<td>LINESTRING (-180 -90, 180 90)</td>
</tr>
<tr><td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
<td>NULL</td>
</tr>
'''

expected_results[all_types_source]["insert"] = """INSERT INTO "table"(bool,tinyint,"smallint","int","bigint",hugeint,uhugeint,utinyint,usmallint,uint,ubigint,bignum,date,"time","timestamp",timestamp_s,timestamp_ms,timestamp_ns,time_tz,timestamp_tz,timestamp_tz_ns,"float","double",dec_4_1,dec_9_4,dec_18_6,dec38_10,uuid,"interval","varchar",blob,"bit",small_enum,medium_enum,large_enum,int_array,double_array,date_array,timestamp_array,timestamptz_array,varchar_array,nested_int_array,"struct",empty_struct,struct_of_arrays,array_of_structs,"map","union",fixed_int_array,fixed_varchar_array,fixed_nested_int_array,fixed_nested_varchar_array,fixed_struct_array,struct_of_fixed_array,fixed_array_of_int_list,list_of_fixed_int_array,time_ns,geometry,"tuple",geography) VALUES('false',-128,-32768,-2147483648,-9223372036854775808,-170141183460469231731687303715884105728,0,0,0,0,0,'-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368','5877642-06-25 (BC)','00:00:00','290309-12-22 (BC) 00:00:00','290309-12-22 (BC) 00:00:00','290309-12-22 (BC) 00:00:00','1677-09-22 00:00:00','00:00:00+15:59:59','290309-12-22 (BC) 00:00:00+00','1677-09-22 00:00:00+00',-3.4028235e+38,-1.7976931348623157e+308,-999.9,-99999.9999,-999999999999.999999,-9999999999999999999999999999.9999999999,'00000000-0000-0000-0000-000000000000','00:00:00','🦆🦆🦆🦆🦆🦆','thisisalongblob\\x00withnullbytes','0010001001011100010101011010111','DUCK_DUCK_ENUM','enum_0','enum_0','[]','[]','[]','[]','[]','[]','[]','{''a'': NULL, ''b'': NULL}','{}','{''a'': NULL, ''b'': NULL}','[]','{}','Frank','[NULL, 2, 3]','[a, NULL, c]','[[NULL, 2, 3], NULL, [NULL, 2, 3]]','[[a, NULL, c], NULL, [a, NULL, c]]','[{''a'': NULL, ''b'': NULL}, {''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}, {''a'': NULL, ''b'': NULL}]','{''a'': [NULL, 2, 3], ''b'': [a, NULL, c]}','[[], [42, 999, NULL, NULL, -42], []]','[[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]','00:00:00','POINT EMPTY','(NULL, NULL)','POINT EMPTY');
INSERT INTO "table"(bool,tinyint,"smallint","int","bigint",hugeint,uhugeint,utinyint,usmallint,uint,ubigint,bignum,date,"time","timestamp",timestamp_s,timestamp_ms,timestamp_ns,time_tz,timestamp_tz,timestamp_tz_ns,"float","double",dec_4_1,dec_9_4,dec_18_6,dec38_10,uuid,"interval","varchar",blob,"bit",small_enum,medium_enum,large_enum,int_array,double_array,date_array,timestamp_array,timestamptz_array,varchar_array,nested_int_array,"struct",empty_struct,struct_of_arrays,array_of_structs,"map","union",fixed_int_array,fixed_varchar_array,fixed_nested_int_array,fixed_nested_varchar_array,fixed_struct_array,struct_of_fixed_array,fixed_array_of_int_list,list_of_fixed_int_array,time_ns,geometry,"tuple",geography) VALUES('true',127,32767,2147483647,9223372036854775807,170141183460469231731687303715884105727,340282366920938463463374607431768211455,255,65535,4294967295,18446744073709551615,'179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368','5881580-07-10','24:00:00','294247-01-10 04:00:54.775806','294247-01-10 04:00:54','294247-01-10 04:00:54.775','2262-04-11 23:47:16.854775806','24:00:00-15:59:59','294247-01-10 04:00:54.775806+00','2262-04-11 23:47:16.854775806+00',3.4028235e+38,1.7976931348623157e+308,999.9,99999.9999,999999999999.999999,9999999999999999999999999999.9999999999,'ffffffff-ffff-ffff-ffff-ffffffffffff','83 years 3 months 999 days 00:16:39.999999','goo\\0se','\\x00\\x00\\x00a','10101','GOOSE','enum_299','enum_69999','[42, 999, NULL, NULL, -42]','[42.0, nan, inf, -inf, NULL, -42.0]','[1970-01-01, infinity, -infinity, NULL, 2022-05-12]','[''1970-01-01 00:00:00'', infinity, -infinity, NULL, ''2022-05-12 16:23:45'']','[''1970-01-01 00:00:00+00'', infinity, -infinity, NULL, ''2022-05-12 23:23:45+00'']','[🦆🦆🦆🦆🦆🦆, goose, NULL, '''']','[[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]','{''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}','{}','{''a'': [42, 999, NULL, NULL, -42], ''b'': [🦆🦆🦆🦆🦆🦆, goose, NULL, '''']}','[{''a'': NULL, ''b'': NULL}, {''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}, NULL]','{key1=🦆🦆🦆🦆🦆🦆, key2=goose}','5','[4, 5, 6]','[d, e, f]','[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]','[[d, e, f], [a, NULL, c], [d, e, f]]','[{''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}, {''a'': NULL, ''b'': NULL}, {''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}]','{''a'': [4, 5, 6], ''b'': [d, e, f]}','[[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]','[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]','24:00:00','GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)','(42, 🦆🦆🦆🦆🦆🦆)','LINESTRING (-180 -90, 180 90)');
INSERT INTO "table"(bool,tinyint,"smallint","int","bigint",hugeint,uhugeint,utinyint,usmallint,uint,ubigint,bignum,date,"time","timestamp",timestamp_s,timestamp_ms,timestamp_ns,time_tz,timestamp_tz,timestamp_tz_ns,"float","double",dec_4_1,dec_9_4,dec_18_6,dec38_10,uuid,"interval","varchar",blob,"bit",small_enum,medium_enum,large_enum,int_array,double_array,date_array,timestamp_array,timestamptz_array,varchar_array,nested_int_array,"struct",empty_struct,struct_of_arrays,array_of_structs,"map","union",fixed_int_array,fixed_varchar_array,fixed_nested_int_array,fixed_nested_varchar_array,fixed_struct_array,struct_of_fixed_array,fixed_array_of_int_list,list_of_fixed_int_array,time_ns,geometry,"tuple",geography) VALUES(NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL);
"""

expected_results[all_types_source]["json"] = '''[{"bool":false,"tinyint":-128,"smallint":-32768,"int":-2147483648,"bigint":-9223372036854775808,"hugeint":"-170141183460469231731687303715884105728","uhugeint":"0","utinyint":0,"usmallint":0,"uint":0,"ubigint":"0","bignum":"-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368","date":"5877642-06-25 (BC)","time":"00:00:00","timestamp":"290309-12-22 (BC) 00:00:00","timestamp_s":"290309-12-22 (BC) 00:00:00","timestamp_ms":"290309-12-22 (BC) 00:00:00","timestamp_ns":"1677-09-22 00:00:00","time_tz":"00:00:00+15:59:59","timestamp_tz":"290309-12-22 (BC) 00:00:00+00","timestamp_tz_ns":"1677-09-22 00:00:00+00","float":-3.4028234663852886e38,"double":-1.7976931348623157e308,"dec_4_1":"-999.9","dec_9_4":"-99999.9999","dec_18_6":"-999999999999.999999","dec38_10":"-9999999999999999999999999999.9999999999","uuid":"00000000-0000-0000-0000-000000000000","interval":"00:00:00","varchar":"🦆🦆🦆🦆🦆🦆","blob":"thisisalongblob\\\\x00withnullbytes","bit":"0010001001011100010101011010111","small_enum":"DUCK_DUCK_ENUM","medium_enum":"enum_0","large_enum":"enum_0","int_array":[],"double_array":[],"date_array":[],"timestamp_array":[],"timestamptz_array":[],"varchar_array":[],"nested_int_array":[],"struct":{"a":null,"b":null},"empty_struct":{},"struct_of_arrays":{"a":null,"b":null},"array_of_structs":[],"map":{},"union":{"name":"Frank"},"fixed_int_array":[null,2,3],"fixed_varchar_array":["a",null,"c"],"fixed_nested_int_array":[[null,2,3],null,[null,2,3]],"fixed_nested_varchar_array":[["a",null,"c"],null,["a",null,"c"]],"fixed_struct_array":[{"a":null,"b":null},{"a":42,"b":"🦆🦆🦆🦆🦆🦆"},{"a":null,"b":null}],"struct_of_fixed_array":{"a":[null,2,3],"b":["a",null,"c"]},"fixed_array_of_int_list":[[],[42,999,null,null,-42],[]],"list_of_fixed_int_array":[[null,2,3],[4,5,6],[null,2,3]],"time_ns":"00:00:00","geometry":"POINT EMPTY","tuple":[null,null],"geography":"POINT EMPTY"},
{"bool":true,"tinyint":127,"smallint":32767,"int":2147483647,"bigint":9223372036854775807,"hugeint":"170141183460469231731687303715884105727","uhugeint":"340282366920938463463374607431768211455","utinyint":255,"usmallint":65535,"uint":4294967295,"ubigint":"18446744073709551615","bignum":"179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368","date":"5881580-07-10","time":"24:00:00","timestamp":"294247-01-10 04:00:54.775806","timestamp_s":"294247-01-10 04:00:54","timestamp_ms":"294247-01-10 04:00:54.775","timestamp_ns":"2262-04-11 23:47:16.854775806","time_tz":"24:00:00-15:59:59","timestamp_tz":"294247-01-10 04:00:54.775806+00","timestamp_tz_ns":"2262-04-11 23:47:16.854775806+00","float":3.4028234663852886e38,"double":1.7976931348623157e308,"dec_4_1":"999.9","dec_9_4":"99999.9999","dec_18_6":"999999999999.999999","dec38_10":"9999999999999999999999999999.9999999999","uuid":"ffffffff-ffff-ffff-ffff-ffffffffffff","interval":"83 years 3 months 999 days 00:16:39.999999","varchar":"goo\\\\0se","blob":"\\\\x00\\\\x00\\\\x00a","bit":"10101","small_enum":"GOOSE","medium_enum":"enum_299","large_enum":"enum_69999","int_array":[42,999,null,null,-42],"double_array":[42.0,NaN,Infinity,-Infinity,null,-42.0],"date_array":["1970-01-01","infinity","-infinity",null,"2022-05-12"],"timestamp_array":["1970-01-01 00:00:00","infinity","-infinity",null,"2022-05-12 16:23:45"],"timestamptz_array":["1970-01-01 00:00:00+00","infinity","-infinity",null,"2022-05-12 23:23:45+00"],"varchar_array":["🦆🦆🦆🦆🦆🦆","goose",null,""],"nested_int_array":[[],[42,999,null,null,-42],null,[],[42,999,null,null,-42]],"struct":{"a":42,"b":"🦆🦆🦆🦆🦆🦆"},"empty_struct":{},"struct_of_arrays":{"a":[42,999,null,null,-42],"b":["🦆🦆🦆🦆🦆🦆","goose",null,""]},"array_of_structs":[{"a":null,"b":null},{"a":42,"b":"🦆🦆🦆🦆🦆🦆"},null],"map":{"key1":"🦆🦆🦆🦆🦆🦆","key2":"goose"},"union":{"age":5},"fixed_int_array":[4,5,6],"fixed_varchar_array":["d","e","f"],"fixed_nested_int_array":[[4,5,6],[null,2,3],[4,5,6]],"fixed_nested_varchar_array":[["d","e","f"],["a",null,"c"],["d","e","f"]],"fixed_struct_array":[{"a":42,"b":"🦆🦆🦆🦆🦆🦆"},{"a":null,"b":null},{"a":42,"b":"🦆🦆🦆🦆🦆🦆"}],"struct_of_fixed_array":{"a":[4,5,6],"b":["d","e","f"]},"fixed_array_of_int_list":[[42,999,null,null,-42],[],[42,999,null,null,-42]],"list_of_fixed_int_array":[[4,5,6],[null,2,3],[4,5,6]],"time_ns":"24:00:00","geometry":"GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)","tuple":[42,"🦆🦆🦆🦆🦆🦆"],"geography":"LINESTRING (-180 -90, 180 90)"},
{"bool":null,"tinyint":null,"smallint":null,"int":null,"bigint":null,"hugeint":null,"uhugeint":null,"utinyint":null,"usmallint":null,"uint":null,"ubigint":null,"bignum":null,"date":null,"time":null,"timestamp":null,"timestamp_s":null,"timestamp_ms":null,"timestamp_ns":null,"time_tz":null,"timestamp_tz":null,"timestamp_tz_ns":null,"float":null,"double":null,"dec_4_1":null,"dec_9_4":null,"dec_18_6":null,"dec38_10":null,"uuid":null,"interval":null,"varchar":null,"blob":null,"bit":null,"small_enum":null,"medium_enum":null,"large_enum":null,"int_array":null,"double_array":null,"date_array":null,"timestamp_array":null,"timestamptz_array":null,"varchar_array":null,"nested_int_array":null,"struct":null,"empty_struct":null,"struct_of_arrays":null,"array_of_structs":null,"map":null,"union":null,"fixed_int_array":null,"fixed_varchar_array":null,"fixed_nested_int_array":null,"fixed_nested_varchar_array":null,"fixed_struct_array":null,"struct_of_fixed_array":null,"fixed_array_of_int_list":null,"list_of_fixed_int_array":null,"time_ns":null,"geometry":null,"tuple":null,"geography":null}]
'''

expected_results[all_types_source]["jsonlines"] = '''{"bool":false,"tinyint":-128,"smallint":-32768,"int":-2147483648,"bigint":-9223372036854775808,"hugeint":"-170141183460469231731687303715884105728","uhugeint":"0","utinyint":0,"usmallint":0,"uint":0,"ubigint":"0","bignum":"-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368","date":"5877642-06-25 (BC)","time":"00:00:00","timestamp":"290309-12-22 (BC) 00:00:00","timestamp_s":"290309-12-22 (BC) 00:00:00","timestamp_ms":"290309-12-22 (BC) 00:00:00","timestamp_ns":"1677-09-22 00:00:00","time_tz":"00:00:00+15:59:59","timestamp_tz":"290309-12-22 (BC) 00:00:00+00","timestamp_tz_ns":"1677-09-22 00:00:00+00","float":-3.4028234663852886e38,"double":-1.7976931348623157e308,"dec_4_1":"-999.9","dec_9_4":"-99999.9999","dec_18_6":"-999999999999.999999","dec38_10":"-9999999999999999999999999999.9999999999","uuid":"00000000-0000-0000-0000-000000000000","interval":"00:00:00","varchar":"🦆🦆🦆🦆🦆🦆","blob":"thisisalongblob\\\\x00withnullbytes","bit":"0010001001011100010101011010111","small_enum":"DUCK_DUCK_ENUM","medium_enum":"enum_0","large_enum":"enum_0","int_array":[],"double_array":[],"date_array":[],"timestamp_array":[],"timestamptz_array":[],"varchar_array":[],"nested_int_array":[],"struct":{"a":null,"b":null},"empty_struct":{},"struct_of_arrays":{"a":null,"b":null},"array_of_structs":[],"map":{},"union":{"name":"Frank"},"fixed_int_array":[null,2,3],"fixed_varchar_array":["a",null,"c"],"fixed_nested_int_array":[[null,2,3],null,[null,2,3]],"fixed_nested_varchar_array":[["a",null,"c"],null,["a",null,"c"]],"fixed_struct_array":[{"a":null,"b":null},{"a":42,"b":"🦆🦆🦆🦆🦆🦆"},{"a":null,"b":null}],"struct_of_fixed_array":{"a":[null,2,3],"b":["a",null,"c"]},"fixed_array_of_int_list":[[],[42,999,null,null,-42],[]],"list_of_fixed_int_array":[[null,2,3],[4,5,6],[null,2,3]],"time_ns":"00:00:00","geometry":"POINT EMPTY","tuple":[null,null],"geography":"POINT EMPTY"}
{"bool":true,"tinyint":127,"smallint":32767,"int":2147483647,"bigint":9223372036854775807,"hugeint":"170141183460469231731687303715884105727","uhugeint":"340282366920938463463374607431768211455","utinyint":255,"usmallint":65535,"uint":4294967295,"ubigint":"18446744073709551615","bignum":"179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368","date":"5881580-07-10","time":"24:00:00","timestamp":"294247-01-10 04:00:54.775806","timestamp_s":"294247-01-10 04:00:54","timestamp_ms":"294247-01-10 04:00:54.775","timestamp_ns":"2262-04-11 23:47:16.854775806","time_tz":"24:00:00-15:59:59","timestamp_tz":"294247-01-10 04:00:54.775806+00","timestamp_tz_ns":"2262-04-11 23:47:16.854775806+00","float":3.4028234663852886e38,"double":1.7976931348623157e308,"dec_4_1":"999.9","dec_9_4":"99999.9999","dec_18_6":"999999999999.999999","dec38_10":"9999999999999999999999999999.9999999999","uuid":"ffffffff-ffff-ffff-ffff-ffffffffffff","interval":"83 years 3 months 999 days 00:16:39.999999","varchar":"goo\\\\0se","blob":"\\\\x00\\\\x00\\\\x00a","bit":"10101","small_enum":"GOOSE","medium_enum":"enum_299","large_enum":"enum_69999","int_array":[42,999,null,null,-42],"double_array":[42.0,NaN,Infinity,-Infinity,null,-42.0],"date_array":["1970-01-01","infinity","-infinity",null,"2022-05-12"],"timestamp_array":["1970-01-01 00:00:00","infinity","-infinity",null,"2022-05-12 16:23:45"],"timestamptz_array":["1970-01-01 00:00:00+00","infinity","-infinity",null,"2022-05-12 23:23:45+00"],"varchar_array":["🦆🦆🦆🦆🦆🦆","goose",null,""],"nested_int_array":[[],[42,999,null,null,-42],null,[],[42,999,null,null,-42]],"struct":{"a":42,"b":"🦆🦆🦆🦆🦆🦆"},"empty_struct":{},"struct_of_arrays":{"a":[42,999,null,null,-42],"b":["🦆🦆🦆🦆🦆🦆","goose",null,""]},"array_of_structs":[{"a":null,"b":null},{"a":42,"b":"🦆🦆🦆🦆🦆🦆"},null],"map":{"key1":"🦆🦆🦆🦆🦆🦆","key2":"goose"},"union":{"age":5},"fixed_int_array":[4,5,6],"fixed_varchar_array":["d","e","f"],"fixed_nested_int_array":[[4,5,6],[null,2,3],[4,5,6]],"fixed_nested_varchar_array":[["d","e","f"],["a",null,"c"],["d","e","f"]],"fixed_struct_array":[{"a":42,"b":"🦆🦆🦆🦆🦆🦆"},{"a":null,"b":null},{"a":42,"b":"🦆🦆🦆🦆🦆🦆"}],"struct_of_fixed_array":{"a":[4,5,6],"b":["d","e","f"]},"fixed_array_of_int_list":[[42,999,null,null,-42],[],[42,999,null,null,-42]],"list_of_fixed_int_array":[[4,5,6],[null,2,3],[4,5,6]],"time_ns":"24:00:00","geometry":"GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)","tuple":[42,"🦆🦆🦆🦆🦆🦆"],"geography":"LINESTRING (-180 -90, 180 90)"}
{"bool":null,"tinyint":null,"smallint":null,"int":null,"bigint":null,"hugeint":null,"uhugeint":null,"utinyint":null,"usmallint":null,"uint":null,"ubigint":null,"bignum":null,"date":null,"time":null,"timestamp":null,"timestamp_s":null,"timestamp_ms":null,"timestamp_ns":null,"time_tz":null,"timestamp_tz":null,"timestamp_tz_ns":null,"float":null,"double":null,"dec_4_1":null,"dec_9_4":null,"dec_18_6":null,"dec38_10":null,"uuid":null,"interval":null,"varchar":null,"blob":null,"bit":null,"small_enum":null,"medium_enum":null,"large_enum":null,"int_array":null,"double_array":null,"date_array":null,"timestamp_array":null,"timestamptz_array":null,"varchar_array":null,"nested_int_array":null,"struct":null,"empty_struct":null,"struct_of_arrays":null,"array_of_structs":null,"map":null,"union":null,"fixed_int_array":null,"fixed_varchar_array":null,"fixed_nested_int_array":null,"fixed_nested_varchar_array":null,"fixed_struct_array":null,"struct_of_fixed_array":null,"fixed_array_of_int_list":null,"list_of_fixed_int_array":null,"time_ns":null,"geometry":null,"tuple":null,"geography":null}
'''

expected_results[all_types_source]["latex"] = '''\\begin{tabular}{|lrrrrllllllllllllllllrrrrrrlllllllllllllllllllllllllllllllll|}
\\hline
bool  & tinyint & smallint &     int     &        bigint        &                 hugeint                  &                uhugeint                 & utinyint & usmallint &    uint    &       ubigint        &                                                                                                                                                         bignum                                                                                                                                                         &        date        &   time   &          timestamp           &        timestamp_s         &        timestamp_ms        &         timestamp_ns          &      time_tz      &          timestamp_tz           &         timestamp_tz_ns          &     float      &          double          & dec_4_1 &   dec_9_4   &       dec_18_6       &                 dec38_10                 &                 uuid                 &                  interval                  &   varchar    &               blob               &               bit               &   small_enum   & medium_enum & large_enum &         int_array          &            double_array             &                     date_array                      &                              timestamp_array                              &                                timestamptz_array                                &          varchar_array          &                            nested_int_array                            &            struct            & empty_struct &                            struct_of_arrays                             &                       array_of_structs                       &               map               & union & fixed_int_array & fixed_varchar_array &        fixed_nested_int_array        &      fixed_nested_varchar_array      &                                  fixed_struct_array                                  &         struct_of_fixed_array          &                   fixed_array_of_int_list                    &         list_of_fixed_int_array         & time_ns  &                                                                                                                                                                                                         geometry                                                                                                                                                                                                          &       tuple        &           geography           \\\\
\\hline
false & -128    & -32768   & -2147483648 & -9223372036854775808 & -170141183460469231731687303715884105728 & 0                                       & 0        & 0         & 0          & 0                    & -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368 & 5877642-06-25 (BC) & 00:00:00 & 290309-12-22 (BC) 00:00:00   & 290309-12-22 (BC) 00:00:00 & 290309-12-22 (BC) 00:00:00 & 1677-09-22 00:00:00           & 00:00:00+15:59:59 & 290309-12-22 (BC) 00:00:00+00   & 1677-09-22 00:00:00+00           & -3.4028235e+38 & -1.7976931348623157e+308 & -999.9  & -99999.9999 & -999999999999.999999 & -9999999999999999999999999999.9999999999 & 00000000-0000-0000-0000-000000000000 & 00:00:00                                   & 🦆🦆🦆🦆🦆🦆 & thisisalongblob\\x00withnullbytes & 0010001001011100010101011010111 & DUCK_DUCK_ENUM & enum_0      & enum_0     & []                         & []                                  & []                                                  & []                                                                        & []                                                                              & []                              & []                                                                     & {'a': NULL, 'b': NULL}       & {}           & {'a': NULL, 'b': NULL}                                                  & []                                                           & {}                              & Frank & [NULL, 2, 3]    & [a, NULL, c]        & [[NULL, 2, 3], NULL, [NULL, 2, 3]]   & [[a, NULL, c], NULL, [a, NULL, c]]   & [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]       & {'a': [NULL, 2, 3], 'b': [a, NULL, c]} & [[], [42, 999, NULL, NULL, -42], []]                         & [[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]] & 00:00:00 & POINT EMPTY                                                                                                                                                                                                                                                                                                                                                                                                               & (NULL, NULL)       & POINT EMPTY                   \\\\
true  & 127     & 32767    & 2147483647  & 9223372036854775807  & 170141183460469231731687303715884105727  & 340282366920938463463374607431768211455 & 255      & 65535     & 4294967295 & 18446744073709551615 & 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368  & 5881580-07-10      & 24:00:00 & 294247-01-10 04:00:54.775806 & 294247-01-10 04:00:54      & 294247-01-10 04:00:54.775  & 2262-04-11 23:47:16.854775806 & 24:00:00-15:59:59 & 294247-01-10 04:00:54.775806+00 & 2262-04-11 23:47:16.854775806+00 & 3.4028235e+38  & 1.7976931348623157e+308  & 999.9   & 99999.9999  & 999999999999.999999  & 9999999999999999999999999999.9999999999  & ffffffff-ffff-ffff-ffff-ffffffffffff & 83 years 3 months 999 days 00:16:39.999999 & goo\\0se      & \\x00\\x00\\x00a                    & 10101                           & GOOSE          & enum_299    & enum_69999 & [42, 999, NULL, NULL, -42] & [42.0, nan, inf, -inf, NULL, -42.0] & [1970-01-01, infinity, -infinity, NULL, 2022-05-12] & ['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45'] & ['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00'] & [🦆🦆🦆🦆🦆🦆, goose, NULL, ''] & [[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]] & {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆} & {}           & {'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']} & [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL] & {key1=🦆🦆🦆🦆🦆🦆, key2=goose} & 5     & [4, 5, 6]       & [d, e, f]           & [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]] & [[d, e, f], [a, NULL, c], [d, e, f]] & [{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}] & {'a': [4, 5, 6], 'b': [d, e, f]}       & [[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]] & [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]    & 24:00:00 & GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY) & (42, 🦆🦆🦆🦆🦆🦆) & LINESTRING (-180 -90, 180 90) \\\\
NULL  & NULL    & NULL     & NULL        & NULL                 & NULL                                     & NULL                                    & NULL     & NULL      & NULL       & NULL                 & NULL                                                                                                                                                                                                                                                                                                                   & NULL               & NULL     & NULL                         & NULL                       & NULL                       & NULL                          & NULL              & NULL                            & NULL                             & NULL           & NULL                     & NULL    & NULL        & NULL                 & NULL                                     & NULL                                 & NULL                                       & NULL         & NULL                             & NULL                            & NULL           & NULL        & NULL       & NULL                       & NULL                                & NULL                                                & NULL                                                                      & NULL                                                                            & NULL                            & NULL                                                                   & NULL                         & NULL         & NULL                                                                    & NULL                                                         & NULL                            & NULL  & NULL            & NULL                & NULL                                 & NULL                                 & NULL                                                                                 & NULL                                   & NULL                                                         & NULL                                    & NULL     & NULL                                                                                                                                                                                                                                                                                                                                                                                                                      & NULL               & NULL                          \\\\
\\hline
\\end{tabular}
'''

expected_results[all_types_source]["line"] = '''                      bool = false
                   tinyint = -128
                  smallint = -32768
                       int = -2147483648
                    bigint = -9223372036854775808
                   hugeint = -170141183460469231731687303715884105728
                  uhugeint = 0
                  utinyint = 0
                 usmallint = 0
                      uint = 0
                   ubigint = 0
                    bignum = -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368
                      date = 5877642-06-25 (BC)
                      time = 00:00:00
                 timestamp = 290309-12-22 (BC) 00:00:00
               timestamp_s = 290309-12-22 (BC) 00:00:00
              timestamp_ms = 290309-12-22 (BC) 00:00:00
              timestamp_ns = 1677-09-22 00:00:00
                   time_tz = 00:00:00+15:59:59
              timestamp_tz = 290309-12-22 (BC) 00:00:00+00
           timestamp_tz_ns = 1677-09-22 00:00:00+00
                     float = -3.4028235e+38
                    double = -1.7976931348623157e+308
                   dec_4_1 = -999.9
                   dec_9_4 = -99999.9999
                  dec_18_6 = -999999999999.999999
                  dec38_10 = -9999999999999999999999999999.9999999999
                      uuid = 00000000-0000-0000-0000-000000000000
                  interval = 00:00:00
                   varchar = 🦆🦆🦆🦆🦆🦆
                      blob = thisisalongblob\\x00withnullbytes
                       bit = 0010001001011100010101011010111
                small_enum = DUCK_DUCK_ENUM
               medium_enum = enum_0
                large_enum = enum_0
                 int_array = []
              double_array = []
                date_array = []
           timestamp_array = []
         timestamptz_array = []
             varchar_array = []
          nested_int_array = []
                    struct = {'a': NULL, 'b': NULL}
              empty_struct = {}
          struct_of_arrays = {'a': NULL, 'b': NULL}
          array_of_structs = []
                       map = {}
                     union = Frank
           fixed_int_array = [NULL, 2, 3]
       fixed_varchar_array = [a, NULL, c]
    fixed_nested_int_array = [[NULL, 2, 3], NULL, [NULL, 2, 3]]
fixed_nested_varchar_array = [[a, NULL, c], NULL, [a, NULL, c]]
        fixed_struct_array = [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]
     struct_of_fixed_array = {'a': [NULL, 2, 3], 'b': [a, NULL, c]}
   fixed_array_of_int_list = [[], [42, 999, NULL, NULL, -42], []]
   list_of_fixed_int_array = [[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]
                   time_ns = 00:00:00
                  geometry = POINT EMPTY
                     tuple = (NULL, NULL)
                 geography = POINT EMPTY

                      bool = true
                   tinyint = 127
                  smallint = 32767
                       int = 2147483647
                    bigint = 9223372036854775807
                   hugeint = 170141183460469231731687303715884105727
                  uhugeint = 340282366920938463463374607431768211455
                  utinyint = 255
                 usmallint = 65535
                      uint = 4294967295
                   ubigint = 18446744073709551615
                    bignum = 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368
                      date = 5881580-07-10
                      time = 24:00:00
                 timestamp = 294247-01-10 04:00:54.775806
               timestamp_s = 294247-01-10 04:00:54
              timestamp_ms = 294247-01-10 04:00:54.775
              timestamp_ns = 2262-04-11 23:47:16.854775806
                   time_tz = 24:00:00-15:59:59
              timestamp_tz = 294247-01-10 04:00:54.775806+00
           timestamp_tz_ns = 2262-04-11 23:47:16.854775806+00
                     float = 3.4028235e+38
                    double = 1.7976931348623157e+308
                   dec_4_1 = 999.9
                   dec_9_4 = 99999.9999
                  dec_18_6 = 999999999999.999999
                  dec38_10 = 9999999999999999999999999999.9999999999
                      uuid = ffffffff-ffff-ffff-ffff-ffffffffffff
                  interval = 83 years 3 months 999 days 00:16:39.999999
                   varchar = goo\\0se
                      blob = \\x00\\x00\\x00a
                       bit = 10101
                small_enum = GOOSE
               medium_enum = enum_299
                large_enum = enum_69999
                 int_array = [42, 999, NULL, NULL, -42]
              double_array = [42.0, nan, inf, -inf, NULL, -42.0]
                date_array = [1970-01-01, infinity, -infinity, NULL, 2022-05-12]
           timestamp_array = ['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45']
         timestamptz_array = ['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00']
             varchar_array = [🦆🦆🦆🦆🦆🦆, goose, NULL, '']
          nested_int_array = [[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]
                    struct = {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}
              empty_struct = {}
          struct_of_arrays = {'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']}
          array_of_structs = [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL]
                       map = {key1=🦆🦆🦆🦆🦆🦆, key2=goose}
                     union = 5
           fixed_int_array = [4, 5, 6]
       fixed_varchar_array = [d, e, f]
    fixed_nested_int_array = [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]
fixed_nested_varchar_array = [[d, e, f], [a, NULL, c], [d, e, f]]
        fixed_struct_array = [{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}]
     struct_of_fixed_array = {'a': [4, 5, 6], 'b': [d, e, f]}
   fixed_array_of_int_list = [[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]
   list_of_fixed_int_array = [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]
                   time_ns = 24:00:00
                  geometry = GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)
                     tuple = (42, 🦆🦆🦆🦆🦆🦆)
                 geography = LINESTRING (-180 -90, 180 90)

                      bool = NULL
                   tinyint = NULL
                  smallint = NULL
                       int = NULL
                    bigint = NULL
                   hugeint = NULL
                  uhugeint = NULL
                  utinyint = NULL
                 usmallint = NULL
                      uint = NULL
                   ubigint = NULL
                    bignum = NULL
                      date = NULL
                      time = NULL
                 timestamp = NULL
               timestamp_s = NULL
              timestamp_ms = NULL
              timestamp_ns = NULL
                   time_tz = NULL
              timestamp_tz = NULL
           timestamp_tz_ns = NULL
                     float = NULL
                    double = NULL
                   dec_4_1 = NULL
                   dec_9_4 = NULL
                  dec_18_6 = NULL
                  dec38_10 = NULL
                      uuid = NULL
                  interval = NULL
                   varchar = NULL
                      blob = NULL
                       bit = NULL
                small_enum = NULL
               medium_enum = NULL
                large_enum = NULL
                 int_array = NULL
              double_array = NULL
                date_array = NULL
           timestamp_array = NULL
         timestamptz_array = NULL
             varchar_array = NULL
          nested_int_array = NULL
                    struct = NULL
              empty_struct = NULL
          struct_of_arrays = NULL
          array_of_structs = NULL
                       map = NULL
                     union = NULL
           fixed_int_array = NULL
       fixed_varchar_array = NULL
    fixed_nested_int_array = NULL
fixed_nested_varchar_array = NULL
        fixed_struct_array = NULL
     struct_of_fixed_array = NULL
   fixed_array_of_int_list = NULL
   list_of_fixed_int_array = NULL
                   time_ns = NULL
                  geometry = NULL
                     tuple = NULL
                 geography = NULL
'''

expected_results[all_types_source]["list"] = '''bool|tinyint|smallint|int|bigint|hugeint|uhugeint|utinyint|usmallint|uint|ubigint|bignum|date|time|timestamp|timestamp_s|timestamp_ms|timestamp_ns|time_tz|timestamp_tz|timestamp_tz_ns|float|double|dec_4_1|dec_9_4|dec_18_6|dec38_10|uuid|interval|varchar|blob|bit|small_enum|medium_enum|large_enum|int_array|double_array|date_array|timestamp_array|timestamptz_array|varchar_array|nested_int_array|struct|empty_struct|struct_of_arrays|array_of_structs|map|union|fixed_int_array|fixed_varchar_array|fixed_nested_int_array|fixed_nested_varchar_array|fixed_struct_array|struct_of_fixed_array|fixed_array_of_int_list|list_of_fixed_int_array|time_ns|geometry|tuple|geography
false|-128|-32768|-2147483648|-9223372036854775808|-170141183460469231731687303715884105728|0|0|0|0|0|-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368|5877642-06-25 (BC)|00:00:00|290309-12-22 (BC) 00:00:00|290309-12-22 (BC) 00:00:00|290309-12-22 (BC) 00:00:00|1677-09-22 00:00:00|00:00:00+15:59:59|290309-12-22 (BC) 00:00:00+00|1677-09-22 00:00:00+00|-3.4028235e+38|-1.7976931348623157e+308|-999.9|-99999.9999|-999999999999.999999|-9999999999999999999999999999.9999999999|00000000-0000-0000-0000-000000000000|00:00:00|🦆🦆🦆🦆🦆🦆|thisisalongblob\\x00withnullbytes|0010001001011100010101011010111|DUCK_DUCK_ENUM|enum_0|enum_0|[]|[]|[]|[]|[]|[]|[]|{'a': NULL, 'b': NULL}|{}|{'a': NULL, 'b': NULL}|[]|{}|Frank|[NULL, 2, 3]|[a, NULL, c]|[[NULL, 2, 3], NULL, [NULL, 2, 3]]|[[a, NULL, c], NULL, [a, NULL, c]]|[{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]|{'a': [NULL, 2, 3], 'b': [a, NULL, c]}|[[], [42, 999, NULL, NULL, -42], []]|[[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]|00:00:00|POINT EMPTY|(NULL, NULL)|POINT EMPTY
true|127|32767|2147483647|9223372036854775807|170141183460469231731687303715884105727|340282366920938463463374607431768211455|255|65535|4294967295|18446744073709551615|179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368|5881580-07-10|24:00:00|294247-01-10 04:00:54.775806|294247-01-10 04:00:54|294247-01-10 04:00:54.775|2262-04-11 23:47:16.854775806|24:00:00-15:59:59|294247-01-10 04:00:54.775806+00|2262-04-11 23:47:16.854775806+00|3.4028235e+38|1.7976931348623157e+308|999.9|99999.9999|999999999999.999999|9999999999999999999999999999.9999999999|ffffffff-ffff-ffff-ffff-ffffffffffff|83 years 3 months 999 days 00:16:39.999999|goo\\0se|\\x00\\x00\\x00a|10101|GOOSE|enum_299|enum_69999|[42, 999, NULL, NULL, -42]|[42.0, nan, inf, -inf, NULL, -42.0]|[1970-01-01, infinity, -infinity, NULL, 2022-05-12]|['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45']|['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00']|[🦆🦆🦆🦆🦆🦆, goose, NULL, '']|[[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]|{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}|{}|{'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']}|[{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL]|{key1=🦆🦆🦆🦆🦆🦆, key2=goose}|5|[4, 5, 6]|[d, e, f]|[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]|[[d, e, f], [a, NULL, c], [d, e, f]]|[{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}]|{'a': [4, 5, 6], 'b': [d, e, f]}|[[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]|[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]|24:00:00|GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)|(42, 🦆🦆🦆🦆🦆🦆)|LINESTRING (-180 -90, 180 90)
NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL|NULL
'''

expected_results[all_types_source]["markdown"] = '''| bool  | tinyint | smallint |     int     |        bigint        |                 hugeint                  |                uhugeint                 | utinyint | usmallint |    uint    |       ubigint        |                                                                                                                                                         bignum                                                                                                                                                         |        date        |   time   |          timestamp           |        timestamp_s         |        timestamp_ms        |         timestamp_ns          |      time_tz      |          timestamp_tz           |         timestamp_tz_ns          |     float      |          double          | dec_4_1 |   dec_9_4   |       dec_18_6       |                 dec38_10                 |                 uuid                 |                  interval                  |   varchar    |               blob               |               bit               |   small_enum   | medium_enum | large_enum |         int_array          |            double_array             |                     date_array                      |                              timestamp_array                              |                                timestamptz_array                                |          varchar_array          |                            nested_int_array                            |            struct            | empty_struct |                            struct_of_arrays                             |                       array_of_structs                       |               map               | union | fixed_int_array | fixed_varchar_array |        fixed_nested_int_array        |      fixed_nested_varchar_array      |                                  fixed_struct_array                                  |         struct_of_fixed_array          |                   fixed_array_of_int_list                    |         list_of_fixed_int_array         | time_ns  |                                                                                                                                                                                                         geometry                                                                                                                                                                                                          |       tuple        |           geography           |
|-------|--------:|---------:|------------:|---------------------:|-----------------------------------------:|----------------------------------------:|---------:|----------:|-----------:|---------------------:|------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------|----------|------------------------------|----------------------------|----------------------------|-------------------------------|-------------------|---------------------------------|----------------------------------|---------------:|-------------------------:|--------:|------------:|---------------------:|-----------------------------------------:|--------------------------------------|--------------------------------------------|--------------|----------------------------------|---------------------------------|----------------|-------------|------------|----------------------------|-------------------------------------|-----------------------------------------------------|---------------------------------------------------------------------------|---------------------------------------------------------------------------------|---------------------------------|------------------------------------------------------------------------|------------------------------|--------------|-------------------------------------------------------------------------|--------------------------------------------------------------|---------------------------------|-------|-----------------|---------------------|--------------------------------------|--------------------------------------|--------------------------------------------------------------------------------------|----------------------------------------|--------------------------------------------------------------|-----------------------------------------|----------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|--------------------|-------------------------------|
| false | -128    | -32768   | -2147483648 | -9223372036854775808 | -170141183460469231731687303715884105728 | 0                                       | 0        | 0         | 0          | 0                    | -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368 | 5877642-06-25 (BC) | 00:00:00 | 290309-12-22 (BC) 00:00:00   | 290309-12-22 (BC) 00:00:00 | 290309-12-22 (BC) 00:00:00 | 1677-09-22 00:00:00           | 00:00:00+15:59:59 | 290309-12-22 (BC) 00:00:00+00   | 1677-09-22 00:00:00+00           | -3.4028235e+38 | -1.7976931348623157e+308 | -999.9  | -99999.9999 | -999999999999.999999 | -9999999999999999999999999999.9999999999 | 00000000-0000-0000-0000-000000000000 | 00:00:00                                   | 🦆🦆🦆🦆🦆🦆 | thisisalongblob\\x00withnullbytes | 0010001001011100010101011010111 | DUCK_DUCK_ENUM | enum_0      | enum_0     | []                         | []                                  | []                                                  | []                                                                        | []                                                                              | []                              | []                                                                     | {'a': NULL, 'b': NULL}       | {}           | {'a': NULL, 'b': NULL}                                                  | []                                                           | {}                              | Frank | [NULL, 2, 3]    | [a, NULL, c]        | [[NULL, 2, 3], NULL, [NULL, 2, 3]]   | [[a, NULL, c], NULL, [a, NULL, c]]   | [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]       | {'a': [NULL, 2, 3], 'b': [a, NULL, c]} | [[], [42, 999, NULL, NULL, -42], []]                         | [[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]] | 00:00:00 | POINT EMPTY                                                                                                                                                                                                                                                                                                                                                                                                               | (NULL, NULL)       | POINT EMPTY                   |
| true  | 127     | 32767    | 2147483647  | 9223372036854775807  | 170141183460469231731687303715884105727  | 340282366920938463463374607431768211455 | 255      | 65535     | 4294967295 | 18446744073709551615 | 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368  | 5881580-07-10      | 24:00:00 | 294247-01-10 04:00:54.775806 | 294247-01-10 04:00:54      | 294247-01-10 04:00:54.775  | 2262-04-11 23:47:16.854775806 | 24:00:00-15:59:59 | 294247-01-10 04:00:54.775806+00 | 2262-04-11 23:47:16.854775806+00 | 3.4028235e+38  | 1.7976931348623157e+308  | 999.9   | 99999.9999  | 999999999999.999999  | 9999999999999999999999999999.9999999999  | ffffffff-ffff-ffff-ffff-ffffffffffff | 83 years 3 months 999 days 00:16:39.999999 | goo\\0se      | \\x00\\x00\\x00a                    | 10101                           | GOOSE          | enum_299    | enum_69999 | [42, 999, NULL, NULL, -42] | [42.0, nan, inf, -inf, NULL, -42.0] | [1970-01-01, infinity, -infinity, NULL, 2022-05-12] | ['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45'] | ['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00'] | [🦆🦆🦆🦆🦆🦆, goose, NULL, ''] | [[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]] | {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆} | {}           | {'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']} | [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL] | {key1=🦆🦆🦆🦆🦆🦆, key2=goose} | 5     | [4, 5, 6]       | [d, e, f]           | [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]] | [[d, e, f], [a, NULL, c], [d, e, f]] | [{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}] | {'a': [4, 5, 6], 'b': [d, e, f]}       | [[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]] | [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]    | 24:00:00 | GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY) | (42, 🦆🦆🦆🦆🦆🦆) | LINESTRING (-180 -90, 180 90) |
| NULL  | NULL    | NULL     | NULL        | NULL                 | NULL                                     | NULL                                    | NULL     | NULL      | NULL       | NULL                 | NULL                                                                                                                                                                                                                                                                                                                   | NULL               | NULL     | NULL                         | NULL                       | NULL                       | NULL                          | NULL              | NULL                            | NULL                             | NULL           | NULL                     | NULL    | NULL        | NULL                 | NULL                                     | NULL                                 | NULL                                       | NULL         | NULL                             | NULL                            | NULL           | NULL        | NULL       | NULL                       | NULL                                | NULL                                                | NULL                                                                      | NULL                                                                            | NULL                            | NULL                                                                   | NULL                         | NULL         | NULL                                                                    | NULL                                                         | NULL                            | NULL  | NULL            | NULL                | NULL                                 | NULL                                 | NULL                                                                                 | NULL                                   | NULL                                                         | NULL                                    | NULL     | NULL                                                                                                                                                                                                                                                                                                                                                                                                                      | NULL               | NULL                          |
'''

expected_results[all_types_source]["quote"] = """'bool','tinyint','smallint','int','bigint','hugeint','uhugeint','utinyint','usmallint','uint','ubigint','bignum','date','time','timestamp','timestamp_s','timestamp_ms','timestamp_ns','time_tz','timestamp_tz','timestamp_tz_ns','float','double','dec_4_1','dec_9_4','dec_18_6','dec38_10','uuid','interval','varchar','blob','bit','small_enum','medium_enum','large_enum','int_array','double_array','date_array','timestamp_array','timestamptz_array','varchar_array','nested_int_array','struct','empty_struct','struct_of_arrays','array_of_structs','map','union','fixed_int_array','fixed_varchar_array','fixed_nested_int_array','fixed_nested_varchar_array','fixed_struct_array','struct_of_fixed_array','fixed_array_of_int_list','list_of_fixed_int_array','time_ns','geometry','tuple','geography'
false,-128,-32768,-2147483648,-9223372036854775808,-170141183460469231731687303715884105728,0,0,0,0,0,'-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368','5877642-06-25 (BC)','00:00:00','290309-12-22 (BC) 00:00:00','290309-12-22 (BC) 00:00:00','290309-12-22 (BC) 00:00:00','1677-09-22 00:00:00','00:00:00+15:59:59','290309-12-22 (BC) 00:00:00+00','1677-09-22 00:00:00+00',-3.4028235e+38,-1.7976931348623157e+308,-999.9,-99999.9999,-999999999999.999999,-9999999999999999999999999999.9999999999,'00000000-0000-0000-0000-000000000000','00:00:00','🦆🦆🦆🦆🦆🦆','thisisalongblob\\x00withnullbytes','0010001001011100010101011010111','DUCK_DUCK_ENUM','enum_0','enum_0','[]','[]','[]','[]','[]','[]','[]','{''a'': NULL, ''b'': NULL}','{}','{''a'': NULL, ''b'': NULL}','[]','{}','Frank','[NULL, 2, 3]','[a, NULL, c]','[[NULL, 2, 3], NULL, [NULL, 2, 3]]','[[a, NULL, c], NULL, [a, NULL, c]]','[{''a'': NULL, ''b'': NULL}, {''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}, {''a'': NULL, ''b'': NULL}]','{''a'': [NULL, 2, 3], ''b'': [a, NULL, c]}','[[], [42, 999, NULL, NULL, -42], []]','[[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]','00:00:00','POINT EMPTY','(NULL, NULL)','POINT EMPTY'
true,127,32767,2147483647,9223372036854775807,170141183460469231731687303715884105727,340282366920938463463374607431768211455,255,65535,4294967295,18446744073709551615,'179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368','5881580-07-10','24:00:00','294247-01-10 04:00:54.775806','294247-01-10 04:00:54','294247-01-10 04:00:54.775','2262-04-11 23:47:16.854775806','24:00:00-15:59:59','294247-01-10 04:00:54.775806+00','2262-04-11 23:47:16.854775806+00',3.4028235e+38,1.7976931348623157e+308,999.9,99999.9999,999999999999.999999,9999999999999999999999999999.9999999999,'ffffffff-ffff-ffff-ffff-ffffffffffff','83 years 3 months 999 days 00:16:39.999999','goo\\0se','\\x00\\x00\\x00a','10101','GOOSE','enum_299','enum_69999','[42, 999, NULL, NULL, -42]','[42.0, nan, inf, -inf, NULL, -42.0]','[1970-01-01, infinity, -infinity, NULL, 2022-05-12]','[''1970-01-01 00:00:00'', infinity, -infinity, NULL, ''2022-05-12 16:23:45'']','[''1970-01-01 00:00:00+00'', infinity, -infinity, NULL, ''2022-05-12 23:23:45+00'']','[🦆🦆🦆🦆🦆🦆, goose, NULL, '''']','[[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]','{''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}','{}','{''a'': [42, 999, NULL, NULL, -42], ''b'': [🦆🦆🦆🦆🦆🦆, goose, NULL, '''']}','[{''a'': NULL, ''b'': NULL}, {''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}, NULL]','{key1=🦆🦆🦆🦆🦆🦆, key2=goose}','5','[4, 5, 6]','[d, e, f]','[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]','[[d, e, f], [a, NULL, c], [d, e, f]]','[{''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}, {''a'': NULL, ''b'': NULL}, {''a'': 42, ''b'': 🦆🦆🦆🦆🦆🦆}]','{''a'': [4, 5, 6], ''b'': [d, e, f]}','[[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]','[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]','24:00:00','GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)','(42, 🦆🦆🦆🦆🦆🦆)','LINESTRING (-180 -90, 180 90)'
NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL
"""

expected_results[all_types_source]["table"] = '''+-------+---------+----------+-------------+----------------------+------------------------------------------+-----------------------------------------+----------+-----------+------------+----------------------+------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+--------------------+----------+------------------------------+----------------------------+----------------------------+-------------------------------+-------------------+---------------------------------+----------------------------------+----------------+--------------------------+---------+-------------+----------------------+------------------------------------------+--------------------------------------+--------------------------------------------+--------------+----------------------------------+---------------------------------+----------------+-------------+------------+----------------------------+-------------------------------------+-----------------------------------------------------+---------------------------------------------------------------------------+---------------------------------------------------------------------------------+---------------------------------+------------------------------------------------------------------------+------------------------------+--------------+-------------------------------------------------------------------------+--------------------------------------------------------------+---------------------------------+-------+-----------------+---------------------+--------------------------------------+--------------------------------------+--------------------------------------------------------------------------------------+----------------------------------------+--------------------------------------------------------------+-----------------------------------------+----------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+--------------------+-------------------------------+
| bool  | tinyint | smallint |     int     |        bigint        |                 hugeint                  |                uhugeint                 | utinyint | usmallint |    uint    |       ubigint        |                                                                                                                                                         bignum                                                                                                                                                         |        date        |   time   |          timestamp           |        timestamp_s         |        timestamp_ms        |         timestamp_ns          |      time_tz      |          timestamp_tz           |         timestamp_tz_ns          |     float      |          double          | dec_4_1 |   dec_9_4   |       dec_18_6       |                 dec38_10                 |                 uuid                 |                  interval                  |   varchar    |               blob               |               bit               |   small_enum   | medium_enum | large_enum |         int_array          |            double_array             |                     date_array                      |                              timestamp_array                              |                                timestamptz_array                                |          varchar_array          |                            nested_int_array                            |            struct            | empty_struct |                            struct_of_arrays                             |                       array_of_structs                       |               map               | union | fixed_int_array | fixed_varchar_array |        fixed_nested_int_array        |      fixed_nested_varchar_array      |                                  fixed_struct_array                                  |         struct_of_fixed_array          |                   fixed_array_of_int_list                    |         list_of_fixed_int_array         | time_ns  |                                                                                                                                                                                                         geometry                                                                                                                                                                                                          |       tuple        |           geography           |
+-------+---------+----------+-------------+----------------------+------------------------------------------+-----------------------------------------+----------+-----------+------------+----------------------+------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+--------------------+----------+------------------------------+----------------------------+----------------------------+-------------------------------+-------------------+---------------------------------+----------------------------------+----------------+--------------------------+---------+-------------+----------------------+------------------------------------------+--------------------------------------+--------------------------------------------+--------------+----------------------------------+---------------------------------+----------------+-------------+------------+----------------------------+-------------------------------------+-----------------------------------------------------+---------------------------------------------------------------------------+---------------------------------------------------------------------------------+---------------------------------+------------------------------------------------------------------------+------------------------------+--------------+-------------------------------------------------------------------------+--------------------------------------------------------------+---------------------------------+-------+-----------------+---------------------+--------------------------------------+--------------------------------------+--------------------------------------------------------------------------------------+----------------------------------------+--------------------------------------------------------------+-----------------------------------------+----------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+--------------------+-------------------------------+
| false | -128    | -32768   | -2147483648 | -9223372036854775808 | -170141183460469231731687303715884105728 | 0                                       | 0        | 0         | 0          | 0                    | -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368 | 5877642-06-25 (BC) | 00:00:00 | 290309-12-22 (BC) 00:00:00   | 290309-12-22 (BC) 00:00:00 | 290309-12-22 (BC) 00:00:00 | 1677-09-22 00:00:00           | 00:00:00+15:59:59 | 290309-12-22 (BC) 00:00:00+00   | 1677-09-22 00:00:00+00           | -3.4028235e+38 | -1.7976931348623157e+308 | -999.9  | -99999.9999 | -999999999999.999999 | -9999999999999999999999999999.9999999999 | 00000000-0000-0000-0000-000000000000 | 00:00:00                                   | 🦆🦆🦆🦆🦆🦆 | thisisalongblob\\x00withnullbytes | 0010001001011100010101011010111 | DUCK_DUCK_ENUM | enum_0      | enum_0     | []                         | []                                  | []                                                  | []                                                                        | []                                                                              | []                              | []                                                                     | {'a': NULL, 'b': NULL}       | {}           | {'a': NULL, 'b': NULL}                                                  | []                                                           | {}                              | Frank | [NULL, 2, 3]    | [a, NULL, c]        | [[NULL, 2, 3], NULL, [NULL, 2, 3]]   | [[a, NULL, c], NULL, [a, NULL, c]]   | [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]       | {'a': [NULL, 2, 3], 'b': [a, NULL, c]} | [[], [42, 999, NULL, NULL, -42], []]                         | [[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]] | 00:00:00 | POINT EMPTY                                                                                                                                                                                                                                                                                                                                                                                                               | (NULL, NULL)       | POINT EMPTY                   |
| true  | 127     | 32767    | 2147483647  | 9223372036854775807  | 170141183460469231731687303715884105727  | 340282366920938463463374607431768211455 | 255      | 65535     | 4294967295 | 18446744073709551615 | 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368  | 5881580-07-10      | 24:00:00 | 294247-01-10 04:00:54.775806 | 294247-01-10 04:00:54      | 294247-01-10 04:00:54.775  | 2262-04-11 23:47:16.854775806 | 24:00:00-15:59:59 | 294247-01-10 04:00:54.775806+00 | 2262-04-11 23:47:16.854775806+00 | 3.4028235e+38  | 1.7976931348623157e+308  | 999.9   | 99999.9999  | 999999999999.999999  | 9999999999999999999999999999.9999999999  | ffffffff-ffff-ffff-ffff-ffffffffffff | 83 years 3 months 999 days 00:16:39.999999 | goo\\0se      | \\x00\\x00\\x00a                    | 10101                           | GOOSE          | enum_299    | enum_69999 | [42, 999, NULL, NULL, -42] | [42.0, nan, inf, -inf, NULL, -42.0] | [1970-01-01, infinity, -infinity, NULL, 2022-05-12] | ['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45'] | ['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00'] | [🦆🦆🦆🦆🦆🦆, goose, NULL, ''] | [[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]] | {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆} | {}           | {'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']} | [{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL] | {key1=🦆🦆🦆🦆🦆🦆, key2=goose} | 5     | [4, 5, 6]       | [d, e, f]           | [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]] | [[d, e, f], [a, NULL, c], [d, e, f]] | [{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}] | {'a': [4, 5, 6], 'b': [d, e, f]}       | [[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]] | [[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]    | 24:00:00 | GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY) | (42, 🦆🦆🦆🦆🦆🦆) | LINESTRING (-180 -90, 180 90) |
| NULL  | NULL    | NULL     | NULL        | NULL                 | NULL                                     | NULL                                    | NULL     | NULL      | NULL       | NULL                 | NULL                                                                                                                                                                                                                                                                                                                   | NULL               | NULL     | NULL                         | NULL                       | NULL                       | NULL                          | NULL              | NULL                            | NULL                             | NULL           | NULL                     | NULL    | NULL        | NULL                 | NULL                                     | NULL                                 | NULL                                       | NULL         | NULL                             | NULL                            | NULL           | NULL        | NULL       | NULL                       | NULL                                | NULL                                                | NULL                                                                      | NULL                                                                            | NULL                            | NULL                                                                   | NULL                         | NULL         | NULL                                                                    | NULL                                                         | NULL                            | NULL  | NULL            | NULL                | NULL                                 | NULL                                 | NULL                                                                                 | NULL                                   | NULL                                                         | NULL                                    | NULL     | NULL                                                                                                                                                                                                                                                                                                                                                                                                                      | NULL               | NULL                          |
+-------+---------+----------+-------------+----------------------+------------------------------------------+-----------------------------------------+----------+-----------+------------+----------------------+------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+--------------------+----------+------------------------------+----------------------------+----------------------------+-------------------------------+-------------------+---------------------------------+----------------------------------+----------------+--------------------------+---------+-------------+----------------------+------------------------------------------+--------------------------------------+--------------------------------------------+--------------+----------------------------------+---------------------------------+----------------+-------------+------------+----------------------------+-------------------------------------+-----------------------------------------------------+---------------------------------------------------------------------------+---------------------------------------------------------------------------------+---------------------------------+------------------------------------------------------------------------+------------------------------+--------------+-------------------------------------------------------------------------+--------------------------------------------------------------+---------------------------------+-------+-----------------+---------------------+--------------------------------------+--------------------------------------+--------------------------------------------------------------------------------------+----------------------------------------+--------------------------------------------------------------+-----------------------------------------+----------+---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+--------------------+-------------------------------+
'''

expected_results[all_types_source]["tabs"] = '''bool	tinyint	smallint	int	bigint	hugeint	uhugeint	utinyint	usmallint	uint	ubigint	bignum	date	time	timestamp	timestamp_s	timestamp_ms	timestamp_ns	time_tz	timestamp_tz	timestamp_tz_ns	float	double	dec_4_1	dec_9_4	dec_18_6	dec38_10	uuid	interval	varchar	blob	bit	small_enum	medium_enum	large_enum	int_array	double_array	date_array	timestamp_array	timestamptz_array	varchar_array	nested_int_array	struct	empty_struct	struct_of_arrays	array_of_structs	map	union	fixed_int_array	fixed_varchar_array	fixed_nested_int_array	fixed_nested_varchar_array	fixed_struct_array	struct_of_fixed_array	fixed_array_of_int_list	list_of_fixed_int_array	time_ns	geometry	tuple	geography
false	-128	-32768	-2147483648	-9223372036854775808	-170141183460469231731687303715884105728	0	0	0	0	0	-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368	5877642-06-25 (BC)	00:00:00	290309-12-22 (BC) 00:00:00	290309-12-22 (BC) 00:00:00	290309-12-22 (BC) 00:00:00	1677-09-22 00:00:00	00:00:00+15:59:59	290309-12-22 (BC) 00:00:00+00	1677-09-22 00:00:00+00	-3.4028235e+38	-1.7976931348623157e+308	-999.9	-99999.9999	-999999999999.999999	-9999999999999999999999999999.9999999999	00000000-0000-0000-0000-000000000000	00:00:00	🦆🦆🦆🦆🦆🦆	thisisalongblob\\x00withnullbytes	0010001001011100010101011010111	DUCK_DUCK_ENUM	enum_0	enum_0	[]	[]	[]	[]	[]	[]	[]	{'a': NULL, 'b': NULL}	{}	{'a': NULL, 'b': NULL}	[]	{}	Frank	[NULL, 2, 3]	[a, NULL, c]	[[NULL, 2, 3], NULL, [NULL, 2, 3]]	[[a, NULL, c], NULL, [a, NULL, c]]	[{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}]	{'a': [NULL, 2, 3], 'b': [a, NULL, c]}	[[], [42, 999, NULL, NULL, -42], []]	[[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]	00:00:00	POINT EMPTY	(NULL, NULL)	POINT EMPTY
true	127	32767	2147483647	9223372036854775807	170141183460469231731687303715884105727	340282366920938463463374607431768211455	255	65535	4294967295	18446744073709551615	179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368	5881580-07-10	24:00:00	294247-01-10 04:00:54.775806	294247-01-10 04:00:54	294247-01-10 04:00:54.775	2262-04-11 23:47:16.854775806	24:00:00-15:59:59	294247-01-10 04:00:54.775806+00	2262-04-11 23:47:16.854775806+00	3.4028235e+38	1.7976931348623157e+308	999.9	99999.9999	999999999999.999999	9999999999999999999999999999.9999999999	ffffffff-ffff-ffff-ffff-ffffffffffff	83 years 3 months 999 days 00:16:39.999999	goo\\0se	\\x00\\x00\\x00a	10101	GOOSE	enum_299	enum_69999	[42, 999, NULL, NULL, -42]	[42.0, nan, inf, -inf, NULL, -42.0]	[1970-01-01, infinity, -infinity, NULL, 2022-05-12]	['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45']	['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00']	[🦆🦆🦆🦆🦆🦆, goose, NULL, '']	[[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]	{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}	{}	{'a': [42, 999, NULL, NULL, -42], 'b': [🦆🦆🦆🦆🦆🦆, goose, NULL, '']}	[{'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, NULL]	{key1=🦆🦆🦆🦆🦆🦆, key2=goose}	5	[4, 5, 6]	[d, e, f]	[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]	[[d, e, f], [a, NULL, c], [d, e, f]]	[{'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': 🦆🦆🦆🦆🦆🦆}]	{'a': [4, 5, 6], 'b': [d, e, f]}	[[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]	[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]	24:00:00	GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)	(42, 🦆🦆🦆🦆🦆🦆)	LINESTRING (-180 -90, 180 90)
NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL	NULL
'''

expected_results[all_types_source]["tcl"] = '''"bool" "tinyint" "smallint" "int" "bigint" "hugeint" "uhugeint" "utinyint" "usmallint" "uint" "ubigint" "bignum" "date" "time" "timestamp" "timestamp_s" "timestamp_ms" "timestamp_ns" "time_tz" "timestamp_tz" "timestamp_tz_ns" "float" "double" "dec_4_1" "dec_9_4" "dec_18_6" "dec38_10" "uuid" "interval" "varchar" "blob" "bit" "small_enum" "medium_enum" "large_enum" "int_array" "double_array" "date_array" "timestamp_array" "timestamptz_array" "varchar_array" "nested_int_array" "struct" "empty_struct" "struct_of_arrays" "array_of_structs" "map" "union" "fixed_int_array" "fixed_varchar_array" "fixed_nested_int_array" "fixed_nested_varchar_array" "fixed_struct_array" "struct_of_fixed_array" "fixed_array_of_int_list" "list_of_fixed_int_array" "time_ns" "geometry" "tuple" "geography"
"false" "-128" "-32768" "-2147483648" "-9223372036854775808" "-170141183460469231731687303715884105728" "0" "0" "0" "0" "0" "-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368" "5877642-06-25 (BC)" "00:00:00" "290309-12-22 (BC) 00:00:00" "290309-12-22 (BC) 00:00:00" "290309-12-22 (BC) 00:00:00" "1677-09-22 00:00:00" "00:00:00+15:59:59" "290309-12-22 (BC) 00:00:00+00" "1677-09-22 00:00:00+00" "-3.4028235e+38" "-1.7976931348623157e+308" "-999.9" "-99999.9999" "-999999999999.999999" "-9999999999999999999999999999.9999999999" "00000000-0000-0000-0000-000000000000" "00:00:00" "\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206" "thisisalongblob\\\\x00withnullbytes" "0010001001011100010101011010111" "DUCK_DUCK_ENUM" "enum_0" "enum_0" "[]" "[]" "[]" "[]" "[]" "[]" "[]" "{'a': NULL, 'b': NULL}" "{}" "{'a': NULL, 'b': NULL}" "[]" "{}" "Frank" "[NULL, 2, 3]" "[a, NULL, c]" "[[NULL, 2, 3], NULL, [NULL, 2, 3]]" "[[a, NULL, c], NULL, [a, NULL, c]]" "[{'a': NULL, 'b': NULL}, {'a': 42, 'b': \\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206}, {'a': NULL, 'b': NULL}]" "{'a': [NULL, 2, 3], 'b': [a, NULL, c]}" "[[], [42, 999, NULL, NULL, -42], []]" "[[NULL, 2, 3], [4, 5, 6], [NULL, 2, 3]]" "00:00:00" "POINT EMPTY" "(NULL, NULL)" "POINT EMPTY"
"true" "127" "32767" "2147483647" "9223372036854775807" "170141183460469231731687303715884105727" "340282366920938463463374607431768211455" "255" "65535" "4294967295" "18446744073709551615" "179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368" "5881580-07-10" "24:00:00" "294247-01-10 04:00:54.775806" "294247-01-10 04:00:54" "294247-01-10 04:00:54.775" "2262-04-11 23:47:16.854775806" "24:00:00-15:59:59" "294247-01-10 04:00:54.775806+00" "2262-04-11 23:47:16.854775806+00" "3.4028235e+38" "1.7976931348623157e+308" "999.9" "99999.9999" "999999999999.999999" "9999999999999999999999999999.9999999999" "ffffffff-ffff-ffff-ffff-ffffffffffff" "83 years 3 months 999 days 00:16:39.999999" "goo\\\\0se" "\\\\x00\\\\x00\\\\x00a" "10101" "GOOSE" "enum_299" "enum_69999" "[42, 999, NULL, NULL, -42]" "[42.0, nan, inf, -inf, NULL, -42.0]" "[1970-01-01, infinity, -infinity, NULL, 2022-05-12]" "['1970-01-01 00:00:00', infinity, -infinity, NULL, '2022-05-12 16:23:45']" "['1970-01-01 00:00:00+00', infinity, -infinity, NULL, '2022-05-12 23:23:45+00']" "[\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206, goose, NULL, '']" "[[], [42, 999, NULL, NULL, -42], NULL, [], [42, 999, NULL, NULL, -42]]" "{'a': 42, 'b': \\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206}" "{}" "{'a': [42, 999, NULL, NULL, -42], 'b': [\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206, goose, NULL, '']}" "[{'a': NULL, 'b': NULL}, {'a': 42, 'b': \\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206}, NULL]" "{key1=\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206, key2=goose}" "5" "[4, 5, 6]" "[d, e, f]" "[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]" "[[d, e, f], [a, NULL, c], [d, e, f]]" "[{'a': 42, 'b': \\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206}, {'a': NULL, 'b': NULL}, {'a': 42, 'b': \\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206}]" "{'a': [4, 5, 6], 'b': [d, e, f]}" "[[42, 999, NULL, NULL, -42], [], [42, 999, NULL, NULL, -42]]" "[[4, 5, 6], [NULL, 2, 3], [4, 5, 6]]" "24:00:00" "GEOMETRYCOLLECTION (POINT (1 2), POINT EMPTY, LINESTRING (0 0, 1 1), LINESTRING EMPTY, POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)), POLYGON EMPTY, MULTIPOINT (5 6, EMPTY), MULTILINESTRING ((0 0, 1 1), EMPTY, (2 2, 3 3), EMPTY), MULTILINESTRING EMPTY, MULTIPOLYGON (((0 0, 0 1, 1 1, 1 0, 0 0)), EMPTY, ((0 0, 0 2, 2 2, 2 0, 0 0)), EMPTY), MULTIPOLYGON EMPTY, GEOMETRYCOLLECTION (POINT (5 6)), GEOMETRYCOLLECTION EMPTY)" "(42, \\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206\\360\\237\\246\\206)" "LINESTRING (-180 -90, 180 90)"
"NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL" "NULL"
'''

def get_expected_output(source, mode):
    return expected_results[source][mode]
