import aerospike
import time
import sys

# --- Configuration ---
AEROSPIKE_INTERNAL_IP = '10.150.0.24'  # Your Aerospike node IP
NAMESPACE = 'sptag_data'
SET_NAME = 'vectors'
RECORD_COUNT = 10000

config = {
    'hosts': [(AEROSPIKE_INTERNAL_IP, 3000)],
    'policies': {'timeout': 1000}
}

try:
    client = aerospike.client(config).connect()
except Exception as e:
    print(f"Failed to connect: {e}")
    sys.exit(1)

# --- 1. WRITE TEST ---
print(f"Starting WRITE test for {RECORD_COUNT} records...")
start_time = time.time()

for i in range(RECORD_COUNT):
    key = (NAMESPACE, SET_NAME, f'doc_{i}')
    record_data = {
        'vector_id': i,
        'status': 'active',
        'content': f'This is the payload for document {i}'
    }
    client.put(key, record_data)

write_time = time.time() - start_time
print(f"Write Test Complete: {write_time:.3f} seconds ({RECORD_COUNT / write_time:.0f} ops/sec)\n")

# --- 2. READ & CORRECTNESS TEST ---
print(f"Starting READ and VALIDATION test for {RECORD_COUNT} records...")
start_time = time.time()
errors = 0

for i in range(RECORD_COUNT):
    key = (NAMESPACE, SET_NAME, f'doc_{i}')
    try:
        (key_, meta, record) = client.get(key)
        
        # CORRECTNESS CHECK: Verify the payload matches
        if record['vector_id'] != i or record['content'] != f'This is the payload for document {i}':
            print(f"Data mismatch at doc_{i}!")
            errors += 1
            
    except Exception as e:
        print(f"Failed to read doc_{i}: {e}")
        errors += 1

read_time = time.time() - start_time
print(f"Read Test Complete: {read_time:.3f} seconds ({RECORD_COUNT / read_time:.0f} ops/sec)")

if errors == 0:
    print("SUCCESS: 100% of records read correctly with zero data corruption.\n")
else:
    print(f"FAILURE: Found {errors} data mismatches.\n")

client.close()
