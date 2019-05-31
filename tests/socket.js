// Ensure sandbox gets private 'lo', which is NOT shared with the host.
const {createServer} = require('net');
const {spawn} = require('child_process'); 
const assert = require('assert');

const {SANDALS} = require('./harness');

const server = createServer(socket=>socket.end('XXXXXX\n'))
server.listen(0, '127.0.0.1', ()=>{
    const port = server.address().port;
    const nc = spawn(
        'nc', ['localhost', port],
        {stdio: ['ignore', 'pipe', 'inherit']});
    const ncOutput = [];
    nc.stdout.on('data', data=>ncOutput.push(data));
    nc.on('close', ()=>{
        // nc connected to the server and got the expected response
        assert.equal(Buffer.concat(ncOutput).toString('utf8'), 'XXXXXX\n');
        const sandals = spawn(SANDALS, [], {stdio: ['pipe', 'pipe', 'inherit']});
        const sandalsOutput = [];
        sandals.stdin.end(JSON.stringify({cmd: ['nc', 'localhost', ''+port]}));
        sandals.stdout.on('data', data=>sandalsOutput.push(data));
        sandals.on('close', code=>{
            // nc inside the sandbox was unable to connect
            assert.equal(code, 0);
            const responseJSON = Buffer.concat(sandalsOutput).toString('utf8');
            const response = JSON.parse(responseJSON);
            if (response.status !== 'exited' || response.code === 0)
                assert.fail(responseJSON);
            process.exit();
        });
    });
});
