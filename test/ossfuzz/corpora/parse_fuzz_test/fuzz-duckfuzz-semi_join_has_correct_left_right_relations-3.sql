select * from t1, t2 where a = b and a in (select c from t3);
