import sys
import subprocess
import tempfile
import os
import shutil

if len(sys.argv) < 2:
     raise Exception('need shell binary as parameter')

def test_exception(command, input, stdout, stderr, errmsg):
     print('--- COMMAND --')
     print(' '.join(command))
     print('--- INPUT --')
     print(input)
     print('--- STDOUT --')
     print(stdout)
     print('--- STDERR --')
     print(stderr)
     raise Exception(errmsg)

def test(cmd, out=None, err=None, extra_commands=None, input_file=None):
     command = [sys.argv[1], '--batch', '-init', '/dev/null']
     if extra_commands:
          command += extra_commands
     if input_file:
          command += [cmd]
          res = subprocess.run(command, input=open(input_file, 'rb').read(), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
     else:
          res = subprocess.run(command, input=bytearray(cmd, 'utf8'), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
     stdout = res.stdout.decode('utf8').strip()
     stderr = res.stderr.decode('utf8').strip()

     if out and out not in stdout:
          test_exception(command, cmd, stdout, stderr, 'out test failed')

     if err and err not in stderr:
          test_exception(command, cmd, stdout, stderr, 'err test failed')

     if not err and stderr != '':
          test_exception(command, cmd, stdout, stderr, 'got err test failed')

     if err is None and res.returncode != 0:
          test_exception(command, cmd, stdout, stderr, 'process returned non-zero exit code but no error was specified')


def tf():
	return tempfile.mktemp().replace('\\','/')

# basic test
test('select \'asdf\' as a;', out='asdf')

test('select * from range(10000);', out='9999')

# test pragma
test("""
.mode csv
.headers off
.sep |
CREATE TABLE t0(c0 INT);
PRAGMA table_info('t0');
""", out='0|c0|INTEGER|false||false')

datafile = tf()
print("42\n84",  file=open(datafile, 'w'))
test('''
CREATE TABLE a (i INTEGER);
.import "%s" a
SELECT SUM(i) FROM a;
''' % datafile, out='126')

# nested types
test('select LIST_VALUE(1, 2);', out='[1, 2]')
test("select STRUCT_PACK(x := 3, y := 3);", out="{'x': 3, 'y': 3}")
test("select STRUCT_PACK(x := 3, y := LIST_VALUE(1, 2));", out="{'x': 3, 'y': [1, 2]}")

test('''
CREATE TABLE a (i STRING);
INSERT INTO a VALUES ('XXXX');
SELECT CAST(i AS INTEGER) FROM a;
''' , err='Could not convert')

test('.auth ON', err='sqlite3_set_authorizer')
test('.auth OFF', err='sqlite3_set_authorizer')
test('.backup %s' % tf(), err='sqlite3_backup_init')

# test newline in value
test('''select 'hello
world' as a;''', out='hello\\nworld')

# test newline in column name
test('''select 42 as "hello
world";''', out='hello\\nworld')

test('''
.bail on
.bail off
.binary on
SELECT 42;
.binary off
SELECT 42;
''')

test('''
.cd %s
.cd %s
''' % (tempfile.gettempdir().replace('\\','/'), os.getcwd().replace('\\','/')))

test('''
CREATE TABLE a (I INTEGER);
.changes on
INSERT INTO a VALUES (42);
DROP TABLE a;
''', out="total_changes: 1")

test('''
CREATE TABLE a (I INTEGER);
.changes on
INSERT INTO a VALUES (42);
INSERT INTO a VALUES (42);
INSERT INTO a VALUES (42);
DROP TABLE a;
''', out="total_changes: 3")

test('''
CREATE TABLE a (I INTEGER);
.changes off
INSERT INTO a VALUES (42);
DROP TABLE a;
''')

# maybe at some point we can do something meaningful here
# test('.dbinfo', err='unable to read database header')

test('''
.echo on
SELECT 42;
''', out="SELECT 42")


test('.exit')
test('.quit')

test('.print asdf', out='asdf')

test('''
.headers on
SELECT 42 as wilbur;
''', out="wilbur")


test('''
.nullvalue wilbur
SELECT NULL;
''', out="wilbur")

test("select 'yo' where 'abc' like 'a%c';", out='yo')

test("select regexp_matches('abc','abc')", out='true')

test('.help', 'Show help text for PATTERN')

test('.load %s' % tf(), err="Error")

# error in streaming result
test('''
SELECT x::INT FROM (SELECT x::VARCHAR x FROM range(10) tbl(x) UNION ALL SELECT 'hello' x) tbl(x);
''', err='Could not convert string')

# this should be fixed
test('.selftest', err='sqlite3_table_column_metadata')

scriptfile = tf()
print("select 42", file=open(scriptfile, 'w'))
test('.read %s' % scriptfile, out='42')


test('.show', out='rowseparator')

test('.limit length 42', err='sqlite3_limit')

# ???
# FIXME
# Parser Error: syntax error at or near "["
# LINE 1: ...concat(quote(s.name) || '.' || quote(f.[from]) || '=?'   || fkey_collate_claus...
#test('.lint fkey-indexes')

test('.timeout', err='sqlite3_busy_timeout')


test('.save %s' % tf(), err='sqlite3_backup_init')
test('.restore %s' % tf(), err='sqlite3_backup_init')


# don't crash plz
test('.vfsinfo')
test('.vfsname')
test('.vfslist')

test('.stats', err="sqlite3_status64")
test('.stats on')
test('.stats off')

test('''
create table test (a int, b varchar);
insert into test values (1, 'hello');
.schema test
''', out="CREATE TABLE test(a INTEGER, b VARCHAR);")

test('''
create table test (a int, b varchar);
insert into test values (1, 'hello');
.schema tes%
''', out="CREATE TABLE test(a INTEGER, b VARCHAR);")

test('''
create table test (a int, b varchar);
insert into test values (1, 'hello');
.schema tes*
''', out="CREATE TABLE test(a INTEGER, b VARCHAR);")

test('''
create table test (a int, b varchar);
CREATE TABLE test2(a INTEGER, b VARCHAR);
.schema
''', out="CREATE TABLE test2(a INTEGER, b VARCHAR);")

test('.fullschema', 'No STAT tables available', '')

test('''
CREATE TABLE asda (i INTEGER);
CREATE TABLE bsdf (i INTEGER);
CREATE TABLE csda (i INTEGER);
.tables
''', out="asda  bsdf  csda")

test('''
CREATE TABLE asda (i INTEGER);
CREATE TABLE bsdf (i INTEGER);
CREATE TABLE csda (i INTEGER);
.tables %da
''', out="asda  csda")

test('.indexes',  out="")

test('''
CREATE TABLE a (i INTEGER);
CREATE INDEX a_idx ON a(i);
.indexes a%
''',  out="a_idx")

# this does not seem to output anything
test('.sha3sum')


test('''
.mode csv
.separator XX
SELECT 42,43;
''', out="42XX43")

test('''
.timer on
SELECT NULL;
''', out="Run Time:")

test('''
.scanstats on
SELECT NULL;
''', err='scanstats')

test('.trace %s\n; SELECT 42;' % tf(), err='sqlite3_trace_v2')

outfile = tf()
test('''
.mode csv
.output %s
SELECT 42;
''' % outfile)
outstr = open(outfile,'rb').read()
if b'42' not in outstr:
     raise Exception('.output test failed')


outfile = tf()
test('''
.once %s
SELECT 43;
''' % outfile)
outstr = open(outfile,'rb').read()
if b'43' not in outstr:
     raise Exception('.once test failed')

# This somehow does not log nor fail. works for me.
test('''
.log %s
SELECT 42;
.log off
''' % tf())

test('''
.mode ascii
SELECT NULL, 42, 'fourty-two', 42.0;
''', out='fourty-two')

test('''
.mode csv
SELECT NULL, 42, 'fourty-two', 42.0;
''', out=',fourty-two,')

test('''
.mode column
.width 10 10 10 10
SELECT NULL, 42, 'fourty-two', 42.0;
''', out='  fourty-two  ')

test('''
.mode html
SELECT NULL, 42, 'fourty-two', 42.0;
''', out='<TD>fourty-two</TD>')

# FIXME sqlite3_column_blob
# test('''
# .mode insert
# SELECT NULL, 42, 'fourty-two', 42.0;
# ''', out='fourty-two')

test('''
.mode line
SELECT NULL, 42, 'fourty-two' x, 42.0;
''', out='x = fourty-two')

test('''
.mode list
SELECT NULL, 42, 'fourty-two', 42.0;
''', out='|fourty-two|')

# FIXME sqlite3_column_blob and %! format specifier
# test('''
# .mode quote
# SELECT NULL, 42, 'fourty-two', 42.0;
# ''', out='fourty-two')

test('''
.mode tabs
SELECT NULL, 42, 'fourty-two', 42.0;
''', out='fourty-two')


db1 = tf()
db2 = tf()

test('''
.open %s
CREATE TABLE t1 (i INTEGER);
INSERT INTO t1 VALUES (42);
.open %s
CREATE TABLE t2 (i INTEGER);
INSERT INTO t2 VALUES (43);
.open %s
SELECT * FROM t1;
''' % (db1, db2, db1), out='42')

# open file that is not a database
duckdb_nonsense_db = 'duckdbtest_nonsensedb.db'
with open(duckdb_nonsense_db, 'w+') as f:
     f.write('blablabla')
test('', err='unable to open', extra_commands=[duckdb_nonsense_db])
os.remove(duckdb_nonsense_db)

# enable_profiling doesn't result in any output
test('''
PRAGMA enable_profiling
''', err="")

# only when we follow it up by an actual query does something get printed to the terminal
test('''
PRAGMA enable_profiling;
SELECT 42;
''', out="42", err="Query Profiling Information")

test('.system echo 42', out="42")
test('.shell echo 42', out="42")

# this fails because db_config is missing
# test('''
# .eqp full
# SELECT 42;
# ''', out="DUMMY_SCAN")

# this fails because the sqlite printf accepts %w for table names

# test('''
# CREATE TABLE a (I INTEGER);
# INSERT INTO a VALUES (42);
# .clone %s
# ''' % tempfile.mktemp())



test('.databases', out='main:')

# .dump test
test('''
CREATE TABLE a (I INTEGER);
.changes off
INSERT INTO a VALUES (42);
.dump
''', 'CREATE TABLE a(i INTEGER)')

test('''
CREATE TABLE a (I INTEGER);
.changes off
INSERT INTO a VALUES (42);
.dump
''', 'COMMIT')

# .dump a specific table
test('''
CREATE TABLE a (I INTEGER);
.changes off
INSERT INTO a VALUES (42);
.dump a
''', 'CREATE TABLE a(i INTEGER);')

# .dump LIKE
test('''
CREATE TABLE a (I INTEGER);
.changes off
INSERT INTO a VALUES (42);
.dump a%
''', 'CREATE TABLE a(i INTEGER);')

# more types, tables and views
test('''
CREATE TABLE a (d DATE, k FLOAT, t TIMESTAMP);
CREATE TABLE b (c INTEGER);
.changes off
INSERT INTO a VALUES (DATE '1992-01-01', 0.3, NOW());
INSERT INTO b SELECT * FROM range(0,10);
.dump
''', 'CREATE TABLE a(d DATE, k FLOAT, t TIMESTAMP);')

# import/export database
target_dir = 'duckdb_shell_test_export_dir'
try:
     shutil.rmtree(target_dir)
except:
     pass
test('''
.mode csv
.changes off
CREATE TABLE integers(i INTEGER);
CREATE TABLE integers2(i INTEGER);
INSERT INTO integers SELECT * FROM range(100);
INSERT INTO integers2 VALUES (1), (3), (99);
EXPORT DATABASE '%s';
DROP TABLE integers;
DROP TABLE integers2;
IMPORT DATABASE '%s';
SELECT SUM(i)*MAX(i) FROM integers JOIN integers2 USING (i);
''' % (target_dir, target_dir), '10197')

shutil.rmtree(target_dir)

# test using .import with a CSV file containing invalid UTF8

duckdb_nonsensecsv = 'duckdbtest_nonsensecsv.csv'
with open(duckdb_nonsensecsv, 'wb+') as f:
     f.write(b'\xFF\n')
test('''
.nullvalue NULL
CREATE TABLE test(i INTEGER);
.import duckdbtest_nonsensecsv.csv test
SELECT * FROM test;
''', out="NULL")
os.remove(duckdb_nonsensecsv)

# .mode latex
test('''
.mode latex
CREATE TABLE a (I INTEGER);
.changes off
INSERT INTO a VALUES (42);
SELECT * FROM a;
''', '\\begin{tabular}')

# .mode trash
test('''
.mode trash
SELECT 1;
''', '')

# dump blobs: FIXME
# test('''
# CREATE TABLE a (b BLOB);
# .changes off
# INSERT INTO a VALUES (DATE '1992-01-01', 0.3, NOW());
# .dump
# ''', 'COMMIT')


# printf %q

# test('''
# CREATE TABLE a (i INTEGER);
# CREATE INDEX a_idx ON a(i);
# .imposter a_idx a_idx_imp
# ''')


# test that sqlite3_complete works somewhat correctly

test('''/*
;
*/
select 42;
''', out='42')

test('''-- this is a comment ;
select 42;
''', out='42')

test('''--;;;;;;
select 42;
''', out='42')

test('/* ;;;;;; */ select 42;', out='42')

if os.name != 'nt':
     test('''
     create table mytable as select * from
     read_csv('/dev/stdin',
       columns=STRUCT_PACK(foo := 'INTEGER', bar := 'INTEGER', baz := 'VARCHAR'),
       AUTO_DETECT='false'
     );
     select * from mytable limit 1;
     ''',
     extra_commands=['-csv', ':memory:'],
     input_file='test/sql/copy/csv/data/test/test.csv',
     out='''foo,bar,baz
0,0," test"''')
