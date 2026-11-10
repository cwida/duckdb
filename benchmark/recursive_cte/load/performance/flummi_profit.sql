CREATE TABLE date_dim(d_date_sk INTEGER, d_date DATE, d_year INTEGER);
INSERT INTO date_dim SELECT k, DATE '2001-12-27' + k::INTEGER, year(DATE '2001-12-27' + k::INTEGER) FROM range(1, 11) t(k);
INSERT INTO date_dim VALUES (11, DATE '2003-01-01', 2003), (12, DATE '2003-01-02', 2003), (13, DATE '2004-01-01', 2004);
CREATE TABLE store(s_store_sk INTEGER);
INSERT INTO store VALUES (1), (2), (3), (4);
CREATE TABLE promotion(p_promo_sk INTEGER, p_channel_email VARCHAR, p_channel_catalog VARCHAR, p_channel_dmail VARCHAR);
INSERT INTO promotion VALUES (1, 'Y', 'N', 'N'), (2, 'N', 'N', 'N'), (3, 'N', 'Y', 'N'), (4, 'N', 'N', 'N');
CREATE TABLE store_sales(ss_sold_date_sk INTEGER, ss_store_sk INTEGER, ss_net_profit DECIMAL(7,2), ss_promo_sk INTEGER);
INSERT INTO store_sales VALUES (2, 1, 10.50, 1), (4, 1, -3.25, 2), (4, 1, 5.00, NULL), (7, 1, 1.10, 1),
  (5, 2, 20.00, 1), (5, 2, -1.00, NULL), (8, 2, NULL, 2), (11, 3, 7.77, NULL);
CREATE TABLE catalog_sales(cs_sold_date_sk INTEGER, cs_promo_sk INTEGER);
INSERT INTO catalog_sales VALUES (1, 3), (3, 3), (2, 2), (6, 2), (9, 4), (11, 3);
CREATE TABLE web_sales(ws_sold_date_sk INTEGER, ws_promo_sk INTEGER);
INSERT INTO web_sales VALUES (3, 1), (5, 1), (6, 2), (7, 2), (10, 2), (12, 2);
