const assert = require('assert');
const { spawnSync } = require('child_process');
const {
    SANDALS,
    test, testAtExit,
    requestInvalid, exited, killed, internalError, timeLimit,
    TmpFile
} = require('./harness');

// Ensure env variables do NOT leak
test('env-leak', ()=>{
    const output = new TmpFile();
    const response = spawnSync(
        SANDALS, [], {
            input: JSON.stringify({
                cmd: ['sh', '-c', 'echo XXXXXX${SECRET}'],
                pipes: [{dest: output, stdout: true}]
            }),
            encoding: 'utf8', env: {SECRET: 42}
        }
    ).stdout;
    const responseJSON = JSON.parse(response);
    if (responseJSON.status !== 'exited' || responseJSON.code !== 0)
        assert.fail(response);
    // We get XXXXXX marker, hence output redirection is working, yet
    // SECRET doesn't come through
    assert.equal(output.read(), 'XXXXXX\n');
});

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
                pipes: [{dest: '/proc/self/fd/26', stdout: true, stderr: true}]
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
        pipes: [{dest: output, stdout: true}]
    });
    assert.equal(output.read(), 'sandals\n');
});

// Ensure only 'lo' interface is available
test('net-interfaces', ()=>{
    const output = new TmpFile();
    exited({
        cmd: ['sh', '-c', "ifconfig -a -s | awk '{ print $1 }'"],
        pipes: [{dest: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), 'Iface\nlo\n');
});

// Ensure sandbox gets private 'lo', which is NOT shared with the host.
'see tests/socket.js';
'not part of the test suite due to a diffetent programming style';

// Ensure ipc is properly isolated:
//   create shared memory with ipcmk,
//   ensure it's listed by ipcs,
//   ensure it's NOT listed by sandboxed ipcs.
test('ipc-isolation', ()=>{
    const id = spawnSync('ipcmk', ['-M', '1'], {encoding: 'utf8'}
    ).stdout.match(/:\s+(\d+)\s*$/)[1];
    assert(id == +id);
    testAtExit(()=>spawnSync('ipcrm', ['-m', id]));
    const idMatcher = new RegExp('\\s'+id+'\\s');
    const normal = spawnSync('ipcs', ['-m'], {encoding: 'utf8'}).stdout;
    if (!normal.match(idMatcher)) assert.fail(normal);
    const sandboxed = new TmpFile();
    exited({cmd: ['ipcs', '-m'], pipes: [{dest: sandboxed, stdout: true}]});
    const sandboxedData = sandboxed.read();
    if (!sandboxedData || sandboxedData.match(idMatcher))
        assert.fail(sandboxedData);
});
