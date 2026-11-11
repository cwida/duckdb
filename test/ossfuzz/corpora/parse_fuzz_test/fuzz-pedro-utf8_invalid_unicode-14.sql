SELECT decode('\xC2\xA3'::BLOB)::VARCHAR ILIKE '£';
