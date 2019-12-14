const assert = require('assert');
const {
    test, requestInvalid, exited, internalError, outputLimit, TmpFile
} = require('./harness');

test('pipesInvalid', ()=>{
    requestInvalid({cmd:['id'], pipes:true}, /^pipes: /);
    requestInvalid({cmd:['id'], pipes:false}, /^pipes: /);
    requestInvalid({cmd:['id'], pipes:null}, /^pipes: /);
    requestInvalid({cmd:['id'], pipes:42}, /^pipes: /);
    requestInvalid({cmd:['id'], pipes:{}}, /^pipes: /);
    requestInvalid({cmd:['id'], pipes:[true]}, /^pipes\[0\]: /);
    requestInvalid({cmd:['id'], pipes:[false]}, /^pipes\[0\]: /);
    requestInvalid({cmd:['id'], pipes:[null]}, /^pipes\[0\]: /);
    requestInvalid({cmd:['id'], pipes:[42]}, /^pipes\[0\]: /);
    requestInvalid({cmd:['id'], pipes:[{}]}, /^pipes\[0\]: /);

    requestInvalid(
        {cmd:['id'], pipes:[{src:'', dest:new TmpFile()}, {}]},
        /^pipes\[1\]: /
    );
});

test('pipes3', ()=>{
    // can use multiple pipes/copyFiles directives simultaneously;
    // ingress file type (regular vs. fifo) is as expected
    for (let k of ['pipes', 'copyFiles']) {
        const o1 = new TmpFile();
        const o2 = new TmpFile();
        const o3 = new TmpFile();
        
        exited({
            mounts: [{type: 'tmpfs', dest: '/tmp'}],
            [k]: [
                {dest: o1, src: '/tmp/o1'},
                {dest: o2, src: '/tmp/o2'},
                {dest: o3, src: '/tmp/o3'},
            ],
            cmd: ['sh', '-c', [
                'echo -n "Hello, "> /tmp/o1',
                'echo world! > /tmp/o2',
                '(test -f /tmp/o3 && echo -n copyFiles || echo -n pipes) > /tmp/o3'
            ].join(';')]
        }, 0);
        assert.equal(o1.read(), 'Hello, ');
        assert.equal(o2.read(), 'world!\n');
        assert.equal(o3.read(), k);
    }
});

test('pipeslimit', ()=>{
    const output = new TmpFile();
    outputLimit({
        cmd: ['yes'],
        pipes: [{dest: output, stdout: true, limit: 10}]
    });
    assert.equal(output.read(), 'y\ny\ny\ny\ny\n');
});

test('copyFilesEarlyFailure', ()=>{
    internalError({
        cmd: ['id'],
        copyFiles: [{dest: new TmpFile(), src: '/no-such-file-or-dir'}]
    });
});

test('copyFilesLimit', ()=>{
    const output = new TmpFile();
    outputLimit({
        cmd: ['echo', '0123456789abcdef'],
        mounts: [{type: 'tmpfs', dest: '/tmp'}],
        copyFiles: [{src: '/tmp/output', dest: output, stdout: true, limit: 4}]
    });
    assert.equal(output.read(), '0123');
});
