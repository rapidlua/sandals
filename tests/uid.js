const assert = require('assert');
const { test, requestInvalid, exited, TmpFile } = require('./harness');

test('uid', ()=>{
    requestInvalid({cmd:['id'], uid:{}}, /^uid: /);
    requestInvalid({cmd:['id'], uid:[]}, /^uid: /);
    requestInvalid({cmd:['id'], uid:''}, /^uid: /);
    requestInvalid({cmd:['id'], uid:-1}, /^uid: /);
    requestInvalid({cmd:['id'], uid:true}, /^uid: /);
    requestInvalid({cmd:['id'], uid:false}, /^uid: /);
    requestInvalid({cmd:['id'], uid:null}, /^uid: /);

    const output = new TmpFile();
    exited({
        cmd: ['id', '-u'],
        pipes: [{dest: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), '0\n');
});

test('uid-custom', ()=>{
    const output = new TmpFile();
    exited({
        cmd: ['sh', '-c', 'id -u; id -g'],
        uid: 99,
        pipes: [{dest: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), '99\n0\n');
});
