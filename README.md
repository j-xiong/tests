Various tests.

## ibv_bw

A bandwidth test for IB Verbs.

### Usage examples

#### RDMA write test
```bash
<server> ibv_bw -t write
<client> ibv_bw -t write <server-name>
```

#### RDMA read test
```bash
<server> ibv_bw -t read
<client> ibv_bw -t read <server-name>
```

#### send/recv test
```bash
<server> ibv_bw -t send
<client> ibv_bw -t send <server-name>
```

#### send/recv test with delayed post of recv requests (by 1000us)
```bash
<server> ibv_bw -t send -D 1000
<client> ibv_bw -t send -D 1000 <server-name>
```

#### send/recv test with delayed post of both send and recv requests (by 1000us)
```bash
<server> ibv_bw -t send -D 1000,1000
<client> ibv_bw -t send -D 1000,1000 <server-name>
```

