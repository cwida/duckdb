select (select 1) a1, (select 3) as a3, c0
        from t0 where a1 + c0 in (select c0 as a1 from t0 where a1 + 1 = a3);
