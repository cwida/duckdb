select
  subtract(
    cast(now() as timestamp),
    cast(ref_0.timestamp as timestamp)) as c10
from
  ts as ref_0;
