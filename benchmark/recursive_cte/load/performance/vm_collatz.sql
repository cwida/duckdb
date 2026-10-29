-- Three-address VM running a Collatz-length program, from the
-- "One WITH RECURSIVE is Worth Many GOTOs" (SIGMOD'21) /
-- "Functional-Style SQL UDFs" (HILDA'22) vm use case
-- (https://github.com/cduta/use-cases, vm/vm-compiled.sql),
-- ported to DuckDB with the register file as columns r1..r7.
CREATE TABLE program(loc INT, opc VARCHAR, reg1 INT, reg2 INT, reg3 INT);
INSERT INTO program VALUES
  ( 0, 'lod', 4, 0   , NULL),
  ( 1, 'lod', 5, 1   , NULL),
  ( 2, 'lod', 6, 2   , NULL),
  ( 3, 'lod', 7, 3   , NULL),
  ( 4, 'mov', 2, 4   , NULL),
  ( 5, 'jeq', 1, 5   , 14  ),
  ( 6, 'add', 2, 2   , 5   ),
  ( 7, 'mod', 3, 1   , 6   ),
  ( 8, 'jeq', 3, 5   , 11  ),
  ( 9, 'div', 1, 1   , 6   ),
  (10, 'jmp', 5, NULL, NULL),
  (11, 'mul', 1, 1   , 7   ),
  (12, 'add', 1, 1   , 5   ),
  (13, 'jmp', 5, NULL, NULL),
  (14, 'hlt', 2, NULL, NULL);

-- One VM run per input value: Collatz length of n.
CREATE TABLE inputs AS SELECT i AS n FROM range(2, 10002) t(i);
