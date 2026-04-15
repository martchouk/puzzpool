# Worker 1 — first /work call, auto-registers
#curl -s -X POST https://puzzle.b58.de/api/v1/work -H "Content-Type: application/json" -d '{"name": "rig-01", "hashrate": 5000000}' | jq
#sleep 5

# Worker 2 — first /work call, auto-registers 
#curl -s -X POST https://puzzle.b58.de/api/v1/work -H "Content-Type: application/json" -d '{"name": "rig-02", "hashrate": 3000000}' | jq
#sleep 5

# Worker 1 — submit job done (use job_id from first response) 
#curl -s -X POST https://puzzle.b58.de/api/v1/submit -H "Content-Type: application/json" -d '{"name": "rig-01", "job_id": 1, "status": "done"}' | jq
#sleep 5

# Worker 2 — submit job done
#curl -s -X POST https://puzzle.b58.de/api/v1/submit  -H "Content-Type: application/json" -d '{"name": "rig-02", "job_id": 2, "status": "done"}' | jq
#sleep 5

# Worker 1 — request next chunk 
#curl -s -X POST https://puzzle.b58.de/api/v1/work -H "Content-Type: application/json" -d '{"name": "rig-01", "hashrate": 5000000}' | jq 
#sleep 5

# Worker 1 — simulate a FOUND result (use the job_id from above) 
#curl -s -X POST https://puzzle.b58.de/api/v1/submit -H "Content-Type: application/json" -d '{"name": "rig-01", "job_id": 3, "status": "FOUND", "found_key": "0x1a2b3c4d", "found_address": "1FeexV6bAHb8ybZjqQMjJrcCrHGW9sb6uF"}' | jq

#Set a test chunk (curl from the server):                                                                                                                                                                           
#Pick any range that you know contains a specific private key. The range just needs to be wide enough that the client's scanner will walk through that key.
curl -s -X POST https://puzzle.b58.de/api/v1/admin/set-test-chunk -H 'Content-Type: application/json' -d '{"start_hex":"0x5fffffffffff1b1e40","end_hex":"0x600000000000e4e1bf"}'

# Reset for test
#rm pool.db*
#sudo systemctl restart puzzpool
#sleep 5
#curl -s -X POST https://puzzle.b58.de/api/v1/admin/set-test-chunk -H 'Content-Type: application/json' -d '{"start_hex":"0x5fffffffffff1b1e40","end_hex":"0x600000000000e4e1bf"}'
#sqlite3 pool.db "SELECT test_start_hex, test_end_hex FROM puzzles WHERE active = 1;"
#sudo systemctl restart puzzpool

                                                        
#Clear it after the test:
#curl -s -X POST http://127.0.0.1:8888/api/v1/admin/set-test-chunk -H 'Content-Type: application/json' -d '{"start_hex":null}'

#curl -s -X POST https://puzzle.b58.de/api/v1/admin/set-test-chunk -H 'Content-Type: application/json' -d '{"start_hex":"0x5fffffffffff1b1e40","end_hex":"0x600000000000e4e1bf"}'
#sqlite3 pool.db "SELECT test_start_hex, test_end_hex FROM puzzles WHERE active = 1;"

