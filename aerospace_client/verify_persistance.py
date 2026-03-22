import aerospike

client = aerospike.client({'hosts': [('10.150.0.24', 3000)]}).connect()
errors = 0

print("Checking for 10,000 restored records...")
for i in range(10000):
    key = ('sptag_data', 'vectors', f'doc_{i}')
    try:
        (key_, meta, record) = client.get(key)
        if record['vector_id'] != i: errors += 1
    except:
        errors += 1

if errors == 0:
    print("PERSISTENCE SUCCESS: All 10,000 records survived the reboot!")
else:
    print(f"PERSISTENCE FAILURE: Missing or corrupted {errors} records.")
    
client.close()
