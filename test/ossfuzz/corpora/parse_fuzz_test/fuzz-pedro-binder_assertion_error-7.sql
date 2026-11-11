select (select 5) a, (select 7) c where (select (select 3) b where (select a) > (select b) and (select c) > (select a));
