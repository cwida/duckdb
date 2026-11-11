CREATE INDEX i0 ON t1 (c1, (decode('\x81\x5C\xE5'::BLOB)::VARCHAR));
