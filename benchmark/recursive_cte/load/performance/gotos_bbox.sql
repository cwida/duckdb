CREATE TYPE vec2 AS STRUCT(x INTEGER, y INTEGER);
CREATE TABLE map(xy vec2, alt INTEGER);
INSERT INTO map(xy, alt)
  SELECT {'x': x + 8, 'y': y}::vec2 AS xy,
         CASE WHEN ((x - 300 / 2.0)^2 / (300 / 2.0 - 1)^2) + ((y - 300 / 2.0)^2 / (300 / 2.0 - 1)^2) <= 1
              THEN 1000 ELSE 0 END AS alt
  FROM   generate_series(-8, 300) AS a(x), generate_series(0, 300) AS b(y);
CREATE TABLE directions(ll BOOLEAN, lr BOOLEAN, ul BOOLEAN, ur BOOLEAN, dir vec2, "track?" BOOLEAN, PRIMARY KEY (ll, lr, ul, ur));
INSERT INTO directions VALUES
  (false,false,false,false, {'x': 1, 'y': 0}, false),
  (false,false,false,true , {'x': 1, 'y': 0}, true ),
  (false,false,true ,false, {'x': 0, 'y': 1}, true ),
  (false,false,true ,true , {'x': 1, 'y': 0}, true ),
  (false,true ,false,false, {'x': 0, 'y':-1}, true ),
  (false,true ,false,true , {'x': 0, 'y':-1}, true ),
  (false,true ,true ,false, {'x': 0, 'y': 1}, true ),
  (false,true ,true ,true , {'x': 0, 'y':-1}, true ),
  (true ,false,false,false, {'x':-1, 'y': 0}, true ),
  (true ,false,false,true , {'x':-1, 'y': 0}, true ),
  (true ,false,true ,false, {'x': 0, 'y': 1}, true ),
  (true ,false,true ,true , {'x': 1, 'y': 0}, true ),
  (true ,true ,false,false, {'x':-1, 'y': 0}, true ),
  (true ,true ,false,true , {'x':-1, 'y': 0}, true ),
  (true ,true ,true ,false, {'x': 0, 'y': 1}, true ),
  (true ,true ,true ,true , NULL           , true );
CREATE TABLE pixels(xy vec2, alt BOOLEAN);
INSERT INTO pixels SELECT m.xy, m.alt > 700 FROM map AS m;
CREATE TABLE squares(xy vec2, ll BOOLEAN, lr BOOLEAN, ul BOOLEAN, ur BOOLEAN);
INSERT INTO squares
  SELECT p0.xy, p0.alt, p1.alt, p2.alt, p3.alt
  FROM   pixels p0, pixels p1, pixels p2, pixels p3
  WHERE  p1.xy = {'x': p0.xy.x + 1, 'y': p0.xy.y + 0}::vec2
  AND    p2.xy = {'x': p0.xy.x + 0, 'y': p0.xy.y + 1}::vec2
  AND    p3.xy = {'x': p0.xy.x + 1, 'y': p0.xy.y + 1}::vec2;
CREATE TYPE box AS STRUCT(lx INTEGER, ly INTEGER, hx INTEGER, hy INTEGER);
CREATE MACRO box_pt(x, y) AS {'lx': x, 'ly': y, 'hx': x, 'hy': y}::box;
CREATE MACRO bound_box(a, b) AS {'lx': least(a.lx, b.lx), 'ly': least(a.ly, b.ly), 'hx': greatest(a.hx, b.hx), 'hy': greatest(a.hy, b.hy)}::box;
CREATE TABLE starts AS
SELECT i, (i % 8)::INTEGER AS x, (1 + (i * 37) % (300 - 1))::INTEGER AS y
FROM   generate_series(1, 40) t(i);
