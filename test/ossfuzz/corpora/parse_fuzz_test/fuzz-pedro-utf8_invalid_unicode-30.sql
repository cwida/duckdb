SELECT upper(decode('\xc1\x80'::BLOB)::VARCHAR);
