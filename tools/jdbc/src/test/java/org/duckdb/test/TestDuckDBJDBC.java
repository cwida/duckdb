package org.duckdb.test;

import java.lang.reflect.Method;
import java.lang.ArrayIndexOutOfBoundsException;
import java.math.BigDecimal;
import java.math.BigInteger;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.DatabaseMetaData;
import java.sql.Date;
import java.sql.Driver;
import java.sql.DriverManager;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.ResultSetMetaData;
import java.sql.Statement;
import java.sql.SQLException;
import java.sql.Time;
import java.sql.Timestamp;
import java.sql.Types;
import java.util.Properties;

import org.duckdb.DuckDBAppender;
import org.duckdb.DuckDBConnection;
import org.duckdb.DuckDBDriver;
import org.duckdb.DuckDBDatabase;

public class TestDuckDBJDBC {

	private static void assertTrue(boolean val) throws Exception {
		if (!val) {
			throw new Exception();
		}
	}

	private static void assertFalse(boolean val) throws Exception {
		assertTrue(!val);
	}

	private static void assertEquals(Object a, Object b) throws Exception {
		if (a == null && b == null) {
			return;
		}
		assertTrue(a.equals(b));
	}

	private static void assertNull(Object a) throws Exception {
		assertTrue(a == null);
	}

	private static void assertEquals(double a, double b, double epsilon) throws Exception {
		assertTrue(Math.abs(a - b) < epsilon);
	}

	private static void fail() throws Exception {
		assertTrue(false);
	}

	static {
		try {
			Class.forName("org.duckdb.DuckDBDriver");
		} catch (ClassNotFoundException e) {
			e.printStackTrace();
		}
	}

	public static void test_connection() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		assertTrue(conn.isValid(0));
		assertFalse(conn.isClosed());

		Statement stmt = conn.createStatement();

		ResultSet rs = stmt.executeQuery("SELECT 42 as a");
		assertFalse(stmt.isClosed());
		assertFalse(rs.isClosed());

		assertTrue(rs.next());
		int res = rs.getInt(1);
		assertEquals(res, 42);
		assertFalse(rs.wasNull());

		res = rs.getInt(1);
		assertEquals(res, 42);
		assertFalse(rs.wasNull());

		res = rs.getInt("a");
		assertEquals(res, 42);
		assertFalse(rs.wasNull());

		try {
			res = rs.getInt(0);
			fail();
		} catch (SQLException e) {
		}

		try {
			res = rs.getInt(2);
			fail();
		} catch (SQLException e) {
		}

		try {
			res = rs.getInt("b");
			fail();
		} catch (SQLException e) {
		}

		assertFalse(rs.next());
		assertFalse(rs.next());

		rs.close();
		rs.close();
		assertTrue(rs.isClosed());

		try {
			res = rs.getInt(1);
			fail();
		} catch (SQLException e) {
		}

		stmt.close();
		stmt.close();
		assertTrue(stmt.isClosed());

		conn.close();
		conn.close();
		assertFalse(conn.isValid(0));
		assertTrue(conn.isClosed());

		try {
			stmt = conn.createStatement();
			fail();
		} catch (SQLException e) {
		}

	}

	public static void test_result() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		ResultSet rs;

		rs = stmt.executeQuery("SELECT CAST(42 AS INTEGER) as a, CAST(4.2 AS DOUBLE) as b");
		ResultSetMetaData meta = rs.getMetaData();
		assertEquals(meta.getColumnCount(), 2);
		assertEquals(meta.getColumnName(1), "a");
		assertEquals(meta.getColumnName(2), "b");
		assertEquals(meta.getColumnTypeName(1), "INTEGER");
		assertEquals(meta.getColumnTypeName(2), "DOUBLE");

		try {
			meta.getColumnName(0);
			fail();
		} catch (ArrayIndexOutOfBoundsException e) {
		}

		try {
			meta.getColumnTypeName(0);
			fail();
		} catch (ArrayIndexOutOfBoundsException e) {
		}

		try {
			meta.getColumnName(3);
			fail();
		} catch (SQLException e) {
		}

		try {
			meta.getColumnTypeName(3);
			fail();
		} catch (SQLException e) {
		}

		assertTrue(rs.next());
		assertEquals(rs.getInt(1), 42);
		assertEquals(rs.getString(1), "42");
		assertEquals(rs.getDouble(1), 42.0, 0.001);
		assertTrue(rs.getObject(1).equals(new Integer(42)));

		assertEquals(rs.getInt("a"), 42);
		assertEquals(rs.getString("a"), "42");
		assertEquals(rs.getDouble("a"), 42.0, 0.001);
		assertTrue(rs.getObject("a").equals(new Integer(42)));

		assertEquals(rs.getInt(2), 4);
		assertEquals(rs.getString(2), "4.2");
		assertEquals(rs.getDouble(2), 4.2, 0.001);
		assertTrue(rs.getObject(2).equals(new Double(4.2)));

		assertEquals(rs.getInt("b"), 4);
		assertEquals(rs.getString("b"), "4.2");
		assertEquals(rs.getDouble("b"), 4.2, 0.001);
		assertTrue(rs.getObject("b").equals(new Double(4.2)));

		assertFalse(rs.next());

		rs.close();

		stmt.close();
		// test duplication
		Connection conn2 = ((DuckDBConnection) conn).duplicate();
		ResultSet rs_conn2 = conn2.createStatement().executeQuery("SELECT 42");
		rs_conn2.next();
		assertEquals(42, rs_conn2.getInt(1));
		rs_conn2.close();
		conn.close();
		conn2.close();
	}

	public static void test_empty_table() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE a (i iNTEGER)");
		ResultSet rs = stmt.executeQuery("SELECT * FROM a");
		assertFalse(rs.next());

		try {
			rs.getObject(1);
			fail();
		} catch (ArrayIndexOutOfBoundsException e) {
		}

		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_broken_next() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE t0(c0 INT8, c1 VARCHAR)");
		stmt.execute(
				"INSERT INTO t0(c1, c0) VALUES (-315929644, 1), (-315929644, -315929644), (-634993846, -1981637379)");
		stmt.execute("INSERT INTO t0(c0, c1) VALUES (-433000283, -433000283)");
		stmt.execute("INSERT INTO t0(c0) VALUES (-995217820)");
		stmt.execute("INSERT INTO t0(c1, c0) VALUES (-315929644, -315929644)");

		ResultSet rs = stmt.executeQuery("SELECT c0 FROM t0");
		while (rs.next()) {
			assertTrue(!rs.getObject(1).equals(null));
		}

		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_multiple_connections() throws Exception {
		Connection conn1 = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt1 = conn1.createStatement();
		Connection conn2 = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt2 = conn2.createStatement();
		Statement stmt3 = conn2.createStatement();

		ResultSet rs1 = stmt1.executeQuery("SELECT 42");
		assertTrue(rs1.next());
		assertEquals(42, rs1.getInt(1));
		rs1.close();

		ResultSet rs2 = stmt2.executeQuery("SELECT 43");
		assertTrue(rs2.next());
		assertEquals(43, rs2.getInt(1));

		ResultSet rs3 = stmt3.executeQuery("SELECT 44");
		assertTrue(rs3.next());
		assertEquals(44, rs3.getInt(1));
		rs3.close();

		// creative closing sequence should also work
		stmt2.close();

		rs3 = stmt3.executeQuery("SELECT 44");
		assertTrue(rs3.next());
		assertEquals(44, rs3.getInt(1));

		stmt2.close();
		rs2.close();
		rs3.close();

		System.gc();
		System.gc();

		// stmt1 still works
		rs1 = stmt1.executeQuery("SELECT 42");
		assertTrue(rs1.next());
		assertEquals(42, rs1.getInt(1));
		rs1.close();

		// stmt3 still works
		rs3 = stmt3.executeQuery("SELECT 42");
		assertTrue(rs3.next());
		assertEquals(42, rs3.getInt(1));
		rs3.close();

		conn2.close();

		stmt3.close();

		rs2 = null;
		rs3 = null;
		stmt2 = null;
		stmt3 = null;
		conn2 = null;

		System.gc();
		System.gc();

		// stmt1 still works
		rs1 = stmt1.executeQuery("SELECT 42");
		assertTrue(rs1.next());
		assertEquals(42, rs1.getInt(1));
		rs1.close();
		conn1.close();
		stmt1.close();
	}

	public static void test_big_data() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();
		int rows = 10000;
		stmt.execute("CREATE TABLE a (i iNTEGER)");
		for (int i = 0; i < rows; i++) {
			stmt.execute("INSERT INTO a VALUES (" + i + ")");
		}

		ResultSet rs = stmt.executeQuery(
				"SELECT CAST(i AS SMALLINT), CAST(i AS INTEGER), CAST(i AS BIGINT), CAST(i AS FLOAT), CAST(i AS DOUBLE), CAST(i as STRING), NULL FROM a");
		int count = 0;
		while (rs.next()) {
			for (int col = 1; col <= 6; col++) {
				assertEquals(rs.getShort(col), (short) count);
				assertFalse(rs.wasNull());
				assertEquals(rs.getInt(col), (int) count);
				assertFalse(rs.wasNull());
				assertEquals(rs.getLong(col), (long) count);
				assertFalse(rs.wasNull());
				assertEquals(rs.getFloat(col), (float) count, 0.001);
				assertFalse(rs.wasNull());
				assertEquals(rs.getDouble(col), (double) count, 0.001);
				assertFalse(rs.wasNull());
				assertEquals(Double.parseDouble(rs.getString(col)), (double) count, 0.001);
				assertFalse(rs.wasNull());
				Object o = rs.getObject(col);
				assertFalse(rs.wasNull());
			}
			short null_short = rs.getShort(7);
			assertTrue(rs.wasNull());
			int null_int = rs.getInt(7);
			assertTrue(rs.wasNull());
			long null_long = rs.getLong(7);
			assertTrue(rs.wasNull());
			float null_float = rs.getFloat(7);
			assertTrue(rs.wasNull());
			double null_double = rs.getDouble(7);
			assertTrue(rs.wasNull());
			String null_string = rs.getString(7);
			assertTrue(rs.wasNull());
			Object null_object = rs.getObject(7);
			assertTrue(rs.wasNull());

			count++;
		}

		assertEquals(rows, count);

		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_crash_bug496() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE t0(c0 BOOLEAN, c1 INT)");
		stmt.execute("CREATE INDEX i0 ON t0(c1, c0)");
		stmt.execute("INSERT INTO t0(c1) VALUES (0)");
		stmt.close();
		conn.close();
	}

	public static void test_tablepragma_bug491() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE t0(c0 INT)");

		ResultSet rs = stmt.executeQuery("PRAGMA table_info('t0')");
		assertTrue(rs.next());

		assertEquals(rs.getInt("cid"), 0);
		assertEquals(rs.getString("name"), "c0");
		assertEquals(rs.getString("type"), "INTEGER");
		assertEquals(rs.getBoolean("notnull"), false);
		rs.getString("dflt_value");
		// assertTrue(rs.wasNull());
		assertEquals(rs.getBoolean("pk"), false);

		assertFalse(rs.next());
		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_nulltruth_bug489() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE t0(c0 INT)");
		stmt.execute("INSERT INTO t0(c0) VALUES (0)");

		ResultSet rs = stmt.executeQuery("SELECT * FROM t0 WHERE NOT(NULL OR TRUE)");
		assertFalse(rs.next());

		rs = stmt.executeQuery("SELECT NOT(NULL OR TRUE)");
		assertTrue(rs.next());
		boolean res = rs.getBoolean(1);
		assertEquals(res, false);
		assertFalse(rs.wasNull());

		rs.close();
		stmt.close();
		conn.close();

	}

	public static void test_empty_prepare_bug500() throws Exception {
		String fileContent = "CREATE TABLE t0(c0 VARCHAR, c1 DOUBLE);\n"
				+ "CREATE TABLE t1(c0 DOUBLE, PRIMARY KEY(c0));\n" + "INSERT INTO t0(c0) VALUES (0), (0), (0), (0);\n"
				+ "INSERT INTO t0(c0) VALUES (NULL), (NULL);\n" + "INSERT INTO t1(c0) VALUES (0), (1);\n" + "\n"
				+ "SELECT t0.c0 FROM t0, t1;";
		Connection con = DriverManager.getConnection("jdbc:duckdb:");
		for (String s : fileContent.split("\n")) {
			Statement st = con.createStatement();
			try {
				st.execute(s);
			} catch (SQLException e) {
				// e.printStackTrace();
			}
		}
		con.close();

	}

	public static void test_borked_string_bug539() throws Exception {
		Connection con = DriverManager.getConnection("jdbc:duckdb:");
		Statement s = con.createStatement();
		s.executeUpdate("CREATE TABLE t0 (c0 VARCHAR)");
		String q = String.format("INSERT INTO t0 VALUES('%c')", 55995);
		s.executeUpdate(q);
		s.close();
		con.close();
	}

	public static void test_prepare_types() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");

		PreparedStatement ps = conn.prepareStatement(
				"SELECT CAST(? AS BOOLEAN) c1, CAST(? AS TINYINT) c2, CAST(? AS SMALLINT) c3, CAST(? AS INTEGER) c4, CAST(? AS BIGINT) c5, CAST(? AS FLOAT) c6, CAST(? AS DOUBLE) c7, CAST(? AS STRING) c8");
		ps.setBoolean(1, true);
		ps.setByte(2, (byte) 42);
		ps.setShort(3, (short) 43);
		ps.setInt(4, 44);
		ps.setLong(5, (long) 45);
		ps.setFloat(6, (float) 4.6);
		ps.setDouble(7, (double) 4.7);
		ps.setString(8, "four eight");

		ResultSet rs = ps.executeQuery();
		assertTrue(rs.next());
		assertEquals(rs.getBoolean(1), true);
		assertEquals(rs.getByte(2), (byte) 42);
		assertEquals(rs.getShort(3), (short) 43);
		assertEquals(rs.getInt(4), 44);
		assertEquals(rs.getLong(5), (long) 45);
		assertEquals(rs.getFloat(6), 4.6, 0.001);
		assertEquals(rs.getDouble(7), 4.7, 0.001);
		assertEquals(rs.getString(8), "four eight");
		rs.close();

		ps.setBoolean(1, false);
		ps.setByte(2, (byte) 82);
		ps.setShort(3, (short) 83);
		ps.setInt(4, 84);
		ps.setLong(5, (long) 85);
		ps.setFloat(6, (float) 8.6);
		ps.setDouble(7, (double) 8.7);
		ps.setString(8, "eight eight");

		rs = ps.executeQuery();
		assertTrue(rs.next());
		assertEquals(rs.getBoolean(1), false);
		assertEquals(rs.getByte(2), (byte) 82);
		assertEquals(rs.getShort(3), (short) 83);
		assertEquals(rs.getInt(4), 84);
		assertEquals(rs.getLong(5), (long) 85);
		assertEquals(rs.getFloat(6), 8.6, 0.001);
		assertEquals(rs.getDouble(7), 8.7, 0.001);
		assertEquals(rs.getString(8), "eight eight");
		rs.close();

		ps.setObject(1, false);
		ps.setObject(2, (byte) 82);
		ps.setObject(3, (short) 83);
		ps.setObject(4, 84);
		ps.setObject(5, (long) 85);
		ps.setObject(6, (float) 8.6);
		ps.setObject(7, (double) 8.7);
		ps.setObject(8, "eight eight");

		rs = ps.executeQuery();
		assertTrue(rs.next());
		assertEquals(rs.getBoolean(1), false);
		assertEquals(rs.getByte(2), (byte) 82);
		assertEquals(rs.getShort(3), (short) 83);
		assertEquals(rs.getInt(4), 84);
		assertEquals(rs.getLong(5), (long) 85);
		assertEquals(rs.getFloat(6), 8.6, 0.001);
		assertEquals(rs.getDouble(7), 8.7, 0.001);
		assertEquals(rs.getString(8), "eight eight");

		ps.setNull(1, 0);
		ps.setNull(2, 0);
		ps.setNull(3, 0);
		ps.setNull(4, 0);
		ps.setNull(5, 0);
		ps.setNull(6, 0);
		ps.setNull(7, 0);
		ps.setNull(8, 0);

		rs = ps.executeQuery();
		assertTrue(rs.next());
		assertEquals(8, rs.getMetaData().getColumnCount());
		for (int c = 1; c <= rs.getMetaData().getColumnCount(); c++) {
			assertNull(rs.getObject(c));
			assertTrue(rs.wasNull());
			assertNull(rs.getString(c));
			assertTrue(rs.wasNull());
		}

		rs.close();
		ps.close();
		conn.close();
	}

	public static void test_prepare_insert() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");

		conn.createStatement()
				.executeUpdate("create table ctstable1 (TYPE_ID int, TYPE_DESC varchar(32), primary key(TYPE_ID))");
		PreparedStatement pStmt1 = conn.prepareStatement("insert into ctstable1 values(?, ?)");
		for (int j = 1; j <= 10; j++) {
			String sTypeDesc = "Type-" + j;
			int newType = j;
			pStmt1.setInt(1, newType);
			pStmt1.setString(2, sTypeDesc);
			int count = pStmt1.executeUpdate();
			assertEquals(count, 1);
		}
		pStmt1.close();

		conn.createStatement().executeUpdate(
				"create table ctstable2 (KEY_ID int, COF_NAME varchar(32), PRICE float, TYPE_ID int, primary key(KEY_ID) )");

		PreparedStatement pStmt = conn.prepareStatement("insert into ctstable2 values(?, ?, ?, ?)");
		for (int i = 1; i <= 10; i++) {
			// Perform the insert(s)
			int newKey = i;
			String newName = "xx" + "-" + i;
			float newPrice = i + (float) .00;
			int newType = i % 5;
			if (newType == 0)
				newType = 5;
			pStmt.setInt(1, newKey);
			pStmt.setString(2, newName);
			pStmt.setFloat(3, newPrice);
			pStmt.setInt(4, newType);
			pStmt.executeUpdate();
		}

		pStmt.close();

		Statement stmt = conn.createStatement();
		ResultSet rs = stmt.executeQuery("SELECT COUNT(*) FROM ctstable1");
		assertTrue(rs.next());
		assertEquals(rs.getInt(1), 10);
		rs.close();

		stmt.executeUpdate("DELETE FROM ctstable1");

		rs = stmt.executeQuery("SELECT COUNT(*) FROM ctstable1");
		assertTrue(rs.next());
		assertEquals(rs.getInt(1), 0);
		rs.close();

		stmt.close();

		conn.close();

	}

	public static void test_read_only() throws Exception {
		Path database_file = Files.createTempFile("duckdb-jdbc-test-", ".duckdb");
		database_file.toFile().delete();

		String jdbc_url = "jdbc:duckdb:" + database_file.toString();
		Properties ro_prop = new Properties();
		ro_prop.setProperty("duckdb.read_only", "true");

		Connection conn_rw = DriverManager.getConnection(jdbc_url);
		assertFalse(conn_rw.isReadOnly());
		Statement stmt = conn_rw.createStatement();
		stmt.execute("CREATE TABLE test (i INTEGER)");
		stmt.execute("INSERT INTO test VALUES (42)");
		stmt.close();
		// we cannot create other connections, be it read-write or read-only right now
		try {
			Connection conn2 = DriverManager.getConnection(jdbc_url);
			fail();
		} catch (Exception e) {
		}

		try {
			Connection conn2 = DriverManager.getConnection(jdbc_url, ro_prop);
			fail();
		} catch (Exception e) {
		}

		// hard shutdown to not have to wait on gc
		DuckDBDatabase db = ((DuckDBConnection) conn_rw).getDatabase();
		conn_rw.close();
		db.shutdown();

		try {
			Statement stmt2 = conn_rw.createStatement();
			stmt2.executeQuery("SELECT 42");
			stmt2.close();
			fail();
		} catch (SQLException e) {
		}

		try {
			Connection conn_dup = ((DuckDBConnection) conn_rw).duplicate();
			conn_dup.close();
			fail();
		} catch (SQLException e) {
		}

		// we can create two parallel read only connections and query them, too
		Connection conn_ro1 = DriverManager.getConnection(jdbc_url, ro_prop);
		Connection conn_ro2 = DriverManager.getConnection(jdbc_url, ro_prop);

		assertTrue(conn_ro1.isReadOnly());
		assertTrue(conn_ro2.isReadOnly());

		Statement stmt1 = conn_ro1.createStatement();
		ResultSet rs1 = stmt1.executeQuery("SELECT * FROM test");
		rs1.next();
		assertEquals(rs1.getInt(1), 42);
		rs1.close();
		stmt1.close();

		Statement stmt2 = conn_ro2.createStatement();
		ResultSet rs2 = stmt2.executeQuery("SELECT * FROM test");
		rs2.next();
		assertEquals(rs2.getInt(1), 42);
		rs2.close();
		stmt2.close();

		conn_ro1.close();
		conn_ro2.close();
	}

	public static void test_hugeint() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		ResultSet rs = stmt.executeQuery(
				"SELECT 42::hugeint hi1, -42::hugeint hi2, 454564646545646546545646545::hugeint hi3, -454564646545646546545646545::hugeint hi4");
		assertTrue(rs.next());
		assertEquals(rs.getObject("hi1"), new BigInteger("42"));
		assertEquals(rs.getObject("hi2"), new BigInteger("-42"));
		assertEquals(rs.getLong("hi1"), 42L);
		assertEquals(rs.getLong("hi2"), -42L);
		assertEquals(rs.getObject("hi3"), new BigInteger("454564646545646546545646545"));
		assertEquals(rs.getObject("hi4"), new BigInteger("-454564646545646546545646545"));
		assertEquals(rs.getBigDecimal("hi1"), new BigDecimal("42"));
		assertEquals(rs.getBigDecimal("hi2"), new BigDecimal("-42"));
		assertEquals(rs.getBigDecimal("hi3"), new BigDecimal("454564646545646546545646545"));
		assertEquals(rs.getBigDecimal("hi4"), new BigDecimal("-454564646545646546545646545"));

		assertFalse(rs.next());
		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_exotic_types() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		ResultSet rs = stmt.executeQuery(
				"SELECT '2019-11-26 21:11:00'::timestamp ts, '2019-11-26'::date dt, interval '5 days' iv, '21:11:00'::time te");
		assertTrue(rs.next());
		assertEquals(rs.getObject("ts"), Timestamp.valueOf("2019-11-26 21:11:00"));
		assertEquals(rs.getTimestamp("ts"), Timestamp.valueOf("2019-11-26 21:11:00"));

		assertEquals(rs.getObject("dt"), Date.valueOf("2019-11-26"));
		assertEquals(rs.getDate("dt"), Date.valueOf("2019-11-26"));

		assertEquals(rs.getObject("iv"), "5 days");

		assertEquals(rs.getObject("te"), Time.valueOf("21:11:00"));
		assertEquals(rs.getTime("te"), Time.valueOf("21:11:00"));

		assertFalse(rs.next());
		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_exotic_nulls() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		ResultSet rs = stmt.executeQuery("SELECT NULL::timestamp ts, NULL::date dt, NULL::time te");
		assertTrue(rs.next());
		assertNull(rs.getObject("ts"));
		assertNull(rs.getTimestamp("ts"));

		assertNull(rs.getObject("dt"));
		assertNull(rs.getDate("dt"));

		assertNull(rs.getObject("te"));
		assertNull(rs.getTime("te"));

		assertFalse(rs.next());
		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_evil_date() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		ResultSet rs = stmt.executeQuery("SELECT '5131-08-05 (BC)'::date d");

		assertTrue(rs.next());
		assertNull(rs.getDate("d"));

		assertFalse(rs.next());
		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_decimal() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		ResultSet rs = stmt.executeQuery("SELECT '1.23'::decimal(3,2) d");

		assertTrue(rs.next());
		assertEquals(rs.getDouble("d"), 1.23);

		assertFalse(rs.next());
		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_schema_reflection() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();
		stmt.execute("CREATE TABLE a (i INTEGER)");
		stmt.execute("CREATE VIEW b AS SELECT i::STRING AS j FROM a");

		DatabaseMetaData md = conn.getMetaData();
		ResultSet rs;

		rs = md.getTableTypes();
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_TYPE"), "BASE TABLE");
		assertEquals(rs.getString(1), "BASE TABLE");

		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_TYPE"), "VIEW");
		assertEquals(rs.getString(1), "VIEW");

		assertFalse(rs.next());
		rs.close();

		rs = md.getCatalogs();
		assertTrue(rs.next());
		assertNull(rs.getObject("TABLE_CAT"));
		assertNull(rs.getObject(1));

		assertFalse(rs.next());
		rs.close();

		rs = md.getSchemas(null, "ma%");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_SCHEM"), "main");
		assertNull(rs.getObject("TABLE_CATALOG"));
		assertEquals(rs.getString(1), "main");
		assertNull(rs.getObject(2));

		assertFalse(rs.next());
		rs.close();

		rs = md.getSchemas(null, "xxx");
		assertFalse(rs.next());
		rs.close();

		rs = md.getTables(null, null, "%", null);

		assertTrue(rs.next());
		assertNull(rs.getObject("TABLE_CAT"));
		assertNull(rs.getObject(1));
		assertEquals(rs.getString("TABLE_SCHEM"), "main");
		assertEquals(rs.getString(2), "main");
		assertEquals(rs.getString("TABLE_NAME"), "a");
		assertEquals(rs.getString(3), "a");
		assertEquals(rs.getString("TABLE_TYPE"), "BASE TABLE");
		assertEquals(rs.getString(4), "BASE TABLE");
		assertNull(rs.getObject("REMARKS"));
		assertNull(rs.getObject(5));
		assertNull(rs.getObject("TYPE_CAT"));
		assertNull(rs.getObject(6));
		assertNull(rs.getObject("TYPE_SCHEM"));
		assertNull(rs.getObject(7));
		assertNull(rs.getObject("TYPE_NAME"));
		assertNull(rs.getObject(8));
		assertNull(rs.getObject("SELF_REFERENCING_COL_NAME"));
		assertNull(rs.getObject(9));
		assertNull(rs.getObject("REF_GENERATION"));
		assertNull(rs.getObject(10));

		assertTrue(rs.next());
		assertNull(rs.getObject("TABLE_CAT"));
		assertNull(rs.getObject(1));
		assertEquals(rs.getString("TABLE_SCHEM"), "main");
		assertEquals(rs.getString(2), "main");
		assertEquals(rs.getString("TABLE_NAME"), "b");
		assertEquals(rs.getString(3), "b");
		assertEquals(rs.getString("TABLE_TYPE"), "VIEW");
		assertEquals(rs.getString(4), "VIEW");
		assertNull(rs.getObject("REMARKS"));
		assertNull(rs.getObject(5));
		assertNull(rs.getObject("TYPE_CAT"));
		assertNull(rs.getObject(6));
		assertNull(rs.getObject("TYPE_SCHEM"));
		assertNull(rs.getObject(7));
		assertNull(rs.getObject("TYPE_NAME"));
		assertNull(rs.getObject(8));
		assertNull(rs.getObject("SELF_REFERENCING_COL_NAME"));
		assertNull(rs.getObject(9));
		assertNull(rs.getObject("REF_GENERATION"));
		assertNull(rs.getObject(10));

		assertFalse(rs.next());
		rs.close();

		rs = md.getTables(null, "main", "a", null);

		assertTrue(rs.next());
		assertNull(rs.getObject("TABLE_CAT"));
		assertNull(rs.getObject(1));
		assertEquals(rs.getString("TABLE_SCHEM"), "main");
		assertEquals(rs.getString(2), "main");
		assertEquals(rs.getString("TABLE_NAME"), "a");
		assertEquals(rs.getString(3), "a");
		assertEquals(rs.getString("TABLE_TYPE"), "BASE TABLE");
		assertEquals(rs.getString(4), "BASE TABLE");
		assertNull(rs.getObject("REMARKS"));
		assertNull(rs.getObject(5));
		assertNull(rs.getObject("TYPE_CAT"));
		assertNull(rs.getObject(6));
		assertNull(rs.getObject("TYPE_SCHEM"));
		assertNull(rs.getObject(7));
		assertNull(rs.getObject("TYPE_NAME"));
		assertNull(rs.getObject(8));
		assertNull(rs.getObject("SELF_REFERENCING_COL_NAME"));
		assertNull(rs.getObject(9));
		assertNull(rs.getObject("REF_GENERATION"));
		assertNull(rs.getObject(10));
		assertFalse(rs.next());

		rs.close();

		rs = md.getTables(null, "main", "xxx", null);
		assertFalse(rs.next());
		rs.close();

		rs = md.getColumns(null, null, "a", null);
		assertTrue(rs.next());
		assertNull(rs.getObject("TABLE_CAT"));
		assertNull(rs.getObject(1));
		assertEquals(rs.getString("TABLE_SCHEM"), "main");
		assertEquals(rs.getString(2), "main");
		assertEquals(rs.getString("TABLE_NAME"), "a");
		assertEquals(rs.getString(3), "a");
		assertEquals(rs.getString("COLUMN_NAME"), "i");
		assertEquals(rs.getString(4), "i");
		assertEquals(rs.getInt("DATA_TYPE"), Types.INTEGER);
		assertEquals(rs.getInt(5), Types.INTEGER);
		assertEquals(rs.getString("TYPE_NAME"), "INTEGER");
		assertEquals(rs.getString(6), "INTEGER");
		assertNull(rs.getObject("COLUMN_SIZE"));
		assertNull(rs.getObject(7));
		assertNull(rs.getObject("BUFFER_LENGTH"));
		assertNull(rs.getObject(8));

		// and so on but whatever

		assertFalse(rs.next());
		rs.close();

		rs = md.getColumns(null, "main", "a", "i");
		assertTrue(rs.next());
		assertNull(rs.getObject("TABLE_CAT"));
		assertNull(rs.getObject(1));
		assertEquals(rs.getString("TABLE_SCHEM"), "main");
		assertEquals(rs.getString(2), "main");
		assertEquals(rs.getString("TABLE_NAME"), "a");
		assertEquals(rs.getString(3), "a");
		assertEquals(rs.getString("COLUMN_NAME"), "i");
		assertEquals(rs.getString(4), "i");
		assertEquals(rs.getInt("DATA_TYPE"), Types.INTEGER);
		assertEquals(rs.getInt(5), Types.INTEGER);
		assertEquals(rs.getString("TYPE_NAME"), "INTEGER");
		assertEquals(rs.getString(6), "INTEGER");
		assertNull(rs.getObject("COLUMN_SIZE"));
		assertNull(rs.getObject(7));
		assertNull(rs.getObject("BUFFER_LENGTH"));
		assertNull(rs.getObject(8));

		assertFalse(rs.next());
		rs.close();

		// try with catalog as well
		rs = md.getColumns(conn.getCatalog(), "main", "a", "i");
		assertTrue(rs.next());
		assertNull(rs.getObject("TABLE_CAT"));
		assertNull(rs.getObject(1));
		assertEquals(rs.getString("TABLE_SCHEM"), "main");
		assertEquals(rs.getString(2), "main");
		assertEquals(rs.getString("TABLE_NAME"), "a");
		assertEquals(rs.getString(3), "a");
		assertEquals(rs.getString("COLUMN_NAME"), "i");
		assertEquals(rs.getString(4), "i");
		assertEquals(rs.getInt("DATA_TYPE"), Types.INTEGER);
		assertEquals(rs.getInt(5), Types.INTEGER);
		assertEquals(rs.getString("TYPE_NAME"), "INTEGER");
		assertEquals(rs.getString(6), "INTEGER");
		assertNull(rs.getObject("COLUMN_SIZE"));
		assertNull(rs.getObject(7));
		assertNull(rs.getObject("BUFFER_LENGTH"));
		assertNull(rs.getObject(8));

		assertFalse(rs.next());
		rs.close();

		rs = md.getColumns(null, "xxx", "a", "i");
		assertFalse(rs.next());
		rs.close();

		rs = md.getColumns(null, "main", "xxx", "i");
		assertFalse(rs.next());
		rs.close();

		rs = md.getColumns(null, "main", "a", "xxx");
		assertFalse(rs.next());
		rs.close();

		conn.close();
	}

	public static void test_connect_wrong_url_bug848() throws Exception {
		Driver d = new DuckDBDriver();
		assertNull(d.connect("jdbc:h2:", null));
	}

	public static void test_parquet_reader() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();
		ResultSet rs = stmt
				.executeQuery("SELECT COUNT(*) FROM parquet_scan('data/parquet-testing/userdata1.parquet')");
		assertTrue(rs.next());
		assertEquals(rs.getInt(1), 1000);
		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_crash_autocommit_bug939() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		PreparedStatement stmt = conn.prepareStatement("CREATE TABLE ontime(flightdate DATE)");
		conn.setAutoCommit(false); // The is the key to getting the crash to happen.
		stmt.executeUpdate();
		stmt.close();
		conn.close();
	}

	public static void test_explain_bug958() throws Exception {
		Connection conn = DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();
		ResultSet rs = stmt.executeQuery("EXPLAIN SELECT 42");
		assertTrue(rs.next());
		assertTrue(rs.getString(1) != null);
		assertTrue(rs.getString(2) != null);

		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_appender_numbers() throws Exception {
		DuckDBConnection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		// int8, int4, int2, int1, float8, float4
		stmt.execute("CREATE TABLE numbers (a BIGINT, b INTEGER, c SMALLINT, d TINYINT, e DOUBLE, f FLOAT)");
		DuckDBAppender appender = conn.createAppender("main", "numbers");

		for (int i = 0; i < 50; i++) {
			appender.beginRow();
			appender.append(Long.MAX_VALUE - i);
			appender.append(Integer.MAX_VALUE - i);
			appender.append(Short.MAX_VALUE - i);
			appender.append(Byte.MAX_VALUE - i);
			appender.append(i);
			appender.append(i);
			appender.endRow();
		}
		appender.close();

		ResultSet rs = stmt.executeQuery("SELECT max(a), max(b), max(c), max(d), max(e), max(f) FROM numbers");
		assertFalse(rs.isClosed());
		assertTrue(rs.next());

		long resA = rs.getLong(1);
		assertEquals(resA, Long.MAX_VALUE);

		int resB = rs.getInt(2);
		assertEquals(resB, Integer.MAX_VALUE);

		short resC = rs.getShort(3);
		assertEquals(resC, Short.MAX_VALUE);

		byte resD = rs.getByte(4);
		assertEquals(resD, Byte.MAX_VALUE);

		double resE = rs.getDouble(5);
		assertEquals(resE, 49.0d);

		float resF = rs.getFloat(6);
		assertEquals(resF, 49.0f);

		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_appender_int_string() throws Exception {
		DuckDBConnection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE data (a INTEGER, s VARCHAR)");
		DuckDBAppender appender = conn.createAppender("main", "data");

		for (int i = 0; i < 1000; i++) {
			appender.beginRow();
			appender.append(i);
			appender.append("str " + i);
			appender.endRow();
		}
		appender.close();

		ResultSet rs = stmt.executeQuery("SELECT max(a), min(s) FROM data");
		assertFalse(rs.isClosed());

		assertTrue(rs.next());
		int resA = rs.getInt(1);
		assertEquals(resA, 999);
		String resB = rs.getString(2);
		assertEquals(resB, "str 0");

		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_appender_table_does_not_exist() throws Exception {
		DuckDBConnection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		try {
			@SuppressWarnings("unused")
			DuckDBAppender appender = conn.createAppender("main", "data");
			fail();
		} catch (SQLException e) {
		}

		stmt.close();
		conn.close();
	}

	public static void test_appender_table_deleted() throws Exception {
		DuckDBConnection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE data (a INTEGER)");
		DuckDBAppender appender = conn.createAppender("main", "data");

		appender.beginRow();
		appender.append(1);
		appender.endRow();

		stmt.execute("DROP TABLE data");

		appender.beginRow();
		appender.append(2);
		appender.endRow();

		try {
			appender.close();
			fail();
		} catch (SQLException e) {
		}

		stmt.close();
		conn.close();
	}

	public static void test_appender_append_too_many_columns() throws Exception {
		DuckDBConnection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE data (a INTEGER)");
		stmt.close();
		DuckDBAppender appender = conn.createAppender("main", "data");

		try {
			appender.beginRow();
			appender.append(1);
			appender.append(2);
			fail();
		} catch (SQLException e) {
		}

		conn.close();
	}

	public static void test_appender_append_too_few_columns() throws Exception {
		DuckDBConnection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE data (a INTEGER, b INTEGER)");
		stmt.close();
		DuckDBAppender appender = conn.createAppender("main", "data");

		try {
			appender.beginRow();
			appender.append(1);
			appender.endRow();
			fail();
		} catch (SQLException e) {
		}

		conn.close();
	}

	public static void test_appender_type_mismatch() throws Exception {
		DuckDBConnection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		stmt.execute("CREATE TABLE data (a INTEGER)");
		DuckDBAppender appender = conn.createAppender("main", "data");

		try {
			appender.beginRow();
			appender.append("str");
			fail();
		} catch (SQLException e) {
		}

		stmt.close();
		conn.close();
	}

	public static void test_get_catalog() throws Exception {
		Connection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		ResultSet rs = conn.getMetaData().getCatalogs();
		assertTrue(rs.next());
		String catalog = rs.getString(1);
		assertFalse(rs.next());
		rs.close();
		assertEquals(conn.getCatalog(), catalog);
		conn.close();
	}

	public static void test_get_table_types_bug1258() throws Exception {
		Connection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();
		stmt.execute("CREATE TABLE a1 (i INTEGER)");
		stmt.execute("CREATE TABLE a2 (i INTEGER)");
		stmt.execute("CREATE TEMPORARY TABLE b (i INTEGER)");
		stmt.execute("CREATE VIEW c AS SELECT * FROM a1");
		stmt.close();

		ResultSet rs = conn.getMetaData().getTables(null, null, null, null);
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "a1");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "a2");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "b");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "c");
		assertFalse(rs.next());
		rs.close();

		rs = conn.getMetaData().getTables(null, null, null, new String[] {});
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "a1");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "a2");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "b");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "c");
		assertFalse(rs.next());
		rs.close();

		rs = conn.getMetaData().getTables(null, null, null, new String[] { "BASE TABLE" });
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "a1");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "a2");
		assertFalse(rs.next());
		rs.close();

		rs = conn.getMetaData().getTables(null, null, null, new String[] { "BASE TABLE", "VIEW" });
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "a1");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "a2");
		assertTrue(rs.next());
		assertEquals(rs.getString("TABLE_NAME"), "c");
		assertFalse(rs.next());
		rs.close();

		rs = conn.getMetaData().getTables(null, null, null, new String[] { "XXXX" });
		assertFalse(rs.next());
		rs.close();

		conn.close();
	}

	public static void test_utf_string_bug1271() throws Exception {
		DuckDBConnection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt = conn.createStatement();

		ResultSet rs = stmt.executeQuery("SELECT 'Mühleisen', '🦆', '🦄ྀི123456789'");
		assertEquals(rs.getMetaData().getColumnName(1), "Mühleisen");
		assertEquals(rs.getMetaData().getColumnName(2), "🦆");
		assertEquals(rs.getMetaData().getColumnName(3), "🦄ྀི123456789");

		assertTrue(rs.next());

		assertEquals(rs.getString(1), "Mühleisen");
		assertEquals(rs.getString(2), "🦆");
		assertEquals(rs.getString(3), "🦄ྀི123456789");

		rs.close();
		stmt.close();
		conn.close();
	}

	public static void test_statement_creation_bug1268() throws Exception {
		DuckDBConnection conn = (DuckDBConnection) DriverManager.getConnection("jdbc:duckdb:");
		Statement stmt;

		stmt = conn.createStatement();
		stmt.close();

		stmt = conn.createStatement(ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY);
		stmt.close();

		stmt = conn.createStatement(ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, 0);
		stmt.close();

		PreparedStatement pstmt;
		pstmt = conn.prepareStatement("SELECT 42");
		pstmt.close();

		pstmt = conn.prepareStatement("SELECT 42", ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY);
		pstmt.close();

		pstmt = conn.prepareStatement("SELECT 42", ResultSet.TYPE_FORWARD_ONLY, ResultSet.CONCUR_READ_ONLY, 0);
		pstmt.close();

		conn.close();
	}

	public static void main(String[] args) throws Exception {
		// Woo I can do reflection too, take this, JUnit!
		Method[] methods = TestDuckDBJDBC.class.getMethods();
		for (Method m : methods) {
			if (m.getName().startsWith("test_")) {
				m.invoke(null);
			}
		}
		System.out.println("OK");
	}
}
