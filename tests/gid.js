const assert = require('assert');
const { test, requestInvalid, exited, TmpFile } = require('./harness');

test('gid', ()=>{
    requestInvalid({cmd:['id'], gid:{}}, /^gid: /);
    requestInvalid({cmd:['id'], gid:[]}, /^gid: /);
    requestInvalid({cmd:['id'], gid:''}, /^gid: /);
    requestInvalid({cmd:['id'], gid:-1}, /^gid: /);
    requestInvalid({cmd:['id'], gid:true}, /^gid: /);
    requestInvalid({cmd:['id'], gid:false}, /^gid: /);
    requestInvalid({cmd:['id'], gid:null}, /^gid: /);

    const output = new TmpFile();
    exited({
        cmd: ['id', '-g'],
        pipes: [{file: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), '0\n');
});

test('gid-custom', ()=>{
    const output = new TmpFile();
    exited({
        cmd: ['sh', '-c', 'id -u; id -g'],
        gid: 99,
        pipes: [{file: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), '0\n99\n');
});
