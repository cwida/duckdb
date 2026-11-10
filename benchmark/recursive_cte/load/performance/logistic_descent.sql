-- Synthetic gradient-descent loop: each iteration recomputes the gradient with a
-- correlated aggregate over a small observation table, so the recursive body carries
-- a base-table scan while every round produces exactly one row.
CREATE TABLE obs AS SELECT * FROM (VALUES
    (-2.0, 0),(-1.5, 0),(-1.0, 0),(-0.5, 1),(0.5, 0),(1.0, 1),(1.5, 1),(2.0, 1)) t(f, y);
