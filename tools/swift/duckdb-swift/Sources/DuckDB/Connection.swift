//
//  DuckDB
//  https://github.com/duckdb/duckdb-swift
//
//  Copyright © 2018-2024 Stichting DuckDB Foundation
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to
//  deal in the Software without restriction, including without limitation the
//  rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
//  sell copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
//  IN THE SOFTWARE.

#if compiler(>=6.0)
internal import Cduckdb
#else
@_implementationOnly import Cduckdb
#endif

/// An object representing a connection to a DuckDB database
///
/// A connection through which a database can be queried
///
/// For each database, you can create one or many connections using
/// ``Database/connect()``.
/// As individual connections are locked during querying it is recommended that
/// in contexts where blocking is undesirable, connections should be accessed
/// asynchronously through an actor or via a background queue.
///
/// The following example creates a new in-memory database and connects to it.
///
/// ```swift
/// do {
///   let database = try Database(store: .inMemory)
///   let connection = try database.connect()
/// }
/// catch {
///   // handle error
/// }
/// ```
public final class Connection: @unchecked Sendable {

  private let database: Database
  private let ptr = UnsafeMutablePointer<duckdb_connection?>.allocate(capacity: 1)

  /// Creates a new connection
  ///
  /// Instantiates a connection through which a database can be queried
  ///
  /// - Parameter database: the database to connect to
  /// - Throws: ``DatabaseError/connectionFailedToInitialize`` if the connection
  ///   failed to instantiate
  public init(database: Database) throws {
    self.database = database
    let status = database.withCDatabase { duckdb_connect($0, ptr) }
    guard status == .success else { throw DatabaseError.connectionFailedToInitialize }
  }

  deinit {
    duckdb_disconnect(ptr)
    ptr.deallocate()
  }
  
  /// Perform a database query
  ///
  /// Takes a raw SQL statement and passes it to the database engine for
  /// execution, returning the result.
  ///
  /// - Note: This is a blocking operation
  /// - Parameter sql: the query string as SQL
  /// - Throws: ``DatabaseError/connectionQueryError(reason:)`` if the query
  ///   did not execute successfully
  /// - Returns: a query result set
  public func query(_ sql: String) throws -> ResultSet {
    try ResultSet(connection: self, sql: sql)
  }
  
  /// Execute a database query
  ///
  /// Takes a raw SQL statement and passes it to the database engine for
  /// execution, ignoring the result.
  ///
  /// - Note: This is a blocking operation
  /// - Parameter sql: the query string as SQL
  /// - Throws: ``DatabaseError/connectionQueryError(reason:)`` if the query
  ///   did not execute successfully
  public func execute(_ sql: String) throws {
    let status = sql.withCString { queryStrPtr in
      duckdb_query(ptr.pointee, queryStrPtr, nil)
    }
    guard status == .success else {
      throw DatabaseError.connectionQueryError(reason: nil)
    }
  }
  
  func withCConnection<T>(_ body: (duckdb_connection?) throws -> T) rethrows -> T {
    try body(ptr.pointee)
  }
}
