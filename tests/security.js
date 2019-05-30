const assert = require('assert');
const { spawnSync } = require('child_process');
const {
    SANDALS,
    test,
    requestInvalid, exited, killed, internalError, timeLimit,
    TmpFile
} = require('./harness');

// Ensure fd-s do not leak
test('fd-leak', ()=>{
    const output = new TmpFile();
    stdio = new Array(27).fill('ignore');
    stdio[0] = 'pipe';
    stdio[26] = output.fd;
    spawnSync(
        SANDALS, [], {
            input: JSON.stringify({
                cmd: ['readlink', '-v', '/proc/self/fd/26'],
                // v v v proves that sandals inherited fd #26
                pipes: [{file: '/proc/self/fd/26', stdout: true, stderr: true}]
            }),
            encoding: 'utf8', stdio
        }
    );
    assert.equal(
        output.read(),
        'readlink: /proc/self/fd/26: No such file or directory\n');
});

// Ensure that sandals is NOT dumpable, sensitive info
// including /proc/1/environ and /proc/1/fd is inaccessible
test('dumpable', ()=>{
    const output = new TmpFile();
    exited({
        cmd: [
            'sh', '-c',
            [
                'cat /proc/1/comm',
                'cat /proc/1/environ',
                'ls /proc/1/fd'
            ].join(';')
        ],
        mounts: [{type: 'proc', dest: '/proc'}],
        pipes: [{file: output, stdout: true}]
    });
    assert.equal(output.read(), 'sandals\n');
});

// Ensure only 'lo' interface is available
test('net-interfaces', ()=>{
    const output = new TmpFile();
    exited({
        cmd: ['sh', '-c', "ifconfig -a -s | awk '{ print $1 }'"],
        pipes: [{file: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), 'Iface\nlo\n');
});
