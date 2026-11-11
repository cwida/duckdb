SELECT upper(decode('\xc0\x80'::BLOB)::VARCHAR);
