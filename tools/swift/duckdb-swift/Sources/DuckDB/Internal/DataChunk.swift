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
import Foundation

final class DataChunk: @unchecked Sendable {
  
  var count: DBInt { duckdb_data_chunk_get_size(ptr.pointee) }
  var columnCount: DBInt { duckdb_data_chunk_get_column_count(ptr.pointee) }
  
  private let ptr = UnsafeMutablePointer<duckdb_data_chunk?>.allocate(capacity: 1)
  
  init(cresult: duckdb_result, index: DBInt) {
    self.ptr.pointee = duckdb_result_get_chunk(cresult, index)!
  }
  
  deinit {
    duckdb_destroy_data_chunk(ptr)
    ptr.deallocate()
  }
  
  func withVector<T>(at index: DBInt, _ body: (Vector) throws -> T) rethrows -> T {
    try body(Vector(duckdb_data_chunk_get_vector(ptr.pointee, index), count: Int(count)))
  }
}
