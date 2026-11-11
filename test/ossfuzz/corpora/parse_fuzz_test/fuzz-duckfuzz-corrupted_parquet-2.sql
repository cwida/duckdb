from read_parquet('data/parquet-testing/broken/fuzzer4360.parquet', filename=true, filename=map {
                    1: {name: 'cool_column', type: 'VARCHAR', default_value: NULL}
                  });
