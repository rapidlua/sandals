const assert = require('assert');
const { accessSync } = require('fs');
const { test, requestInvalid, exited, TmpFile } = require('./harness');

// Poke chroot:
// * mounts work with chroot;
// * new filesystem root hosts a distinct set of directories;
// * not possible to escape the jail with tricks like cd ../..;
// * pipe[*].fifo isn't confused by chroot.
//
test('chroot', ()=>{
    const output = new TmpFile();
    const canary = new TmpFile();
    const mounts = [
        {type: 'tmpfs', dest: '/'},
        {type: 'bind', dest: '/bin', src: '/bin'},
        {type: 'bind', dest: '/lib', src: '/lib'},
        {type: 'bind', dest: '/usr', src: '/usr'},
        {type: 'bind', dest: '/etc', src: '/etc'},
        {type: 'tmpfs', dest: '/tmp'}
    ];
    try {
        accessSync('/lib64');
        mounts.push({type: 'bind', dest: '/lib64', src: '/lib64'});
    } catch (e) {
    }
    exited({
        cmd: ['sh', '-c', 'cd ../../../..; ls; echo XXXXXX > /tmp/@canary'],
        workDir: '.',
        chroot: '/tmp',
        mounts,
        pipes: [
            {file: output, stdout: true, stderr: true},
            {file: canary, fifo: '/tmp/@canary'}
        ]
    });
    assert.deepEqual(
        output.read().split('\n').sort().filter(dir => dir && dir!=='lib64'),
        ['bin', 'etc', 'lib', 'tmp', 'usr']
    );
    assert.equal(canary.read(), 'XXXXXX\n');
});

// Pass values of a wrong type
test('chroot-invalid', ()=>{
    requestInvalid({cmd:['id'], chroot:{}}, /^chroot: /);
    requestInvalid({cmd:['id'], chroot:[]}, /^chroot: /);
    requestInvalid({cmd:['id'], chroot:-1}, /^chroot: /);
    requestInvalid({cmd:['id'], chroot:true}, /^chroot: /);
    requestInvalid({cmd:['id'], chroot:false}, /^chroot: /);
    requestInvalid({cmd:['id'], chroot:null}, /^chroot: /);
});
