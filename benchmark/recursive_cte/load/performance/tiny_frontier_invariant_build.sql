CREATE TABLE edges AS
SELECT
    (i // 10)::BIGINT AS src,
    (((i // 10) * 13 + i % 10 + 7) % 1000000)::BIGINT AS dst
FROM range(10000000) AS t(i);
