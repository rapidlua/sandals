const assert = require('assert');
const { test, requestInvalid, exited, TmpFile } = require('./harness');

test('hostName', ()=>{
    requestInvalid({cmd:['id'], hostName:{}}, /^hostName: /);
    requestInvalid({cmd:['id'], hostName:[]}, /^hostName: /);
    requestInvalid({cmd:['id'], hostName:0}, /^hostName: /);
    requestInvalid({cmd:['id'], hostName:true}, /^hostName: /);
    requestInvalid({cmd:['id'], hostName:false}, /^hostName: /);
    requestInvalid({cmd:['id'], hostName:null}, /^hostName: /);

    const output = new TmpFile();
    exited({
        cmd: ['hostname'],
        pipes: [{file: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), 'sandals\n');
});

test('hostName-custom', ()=>{
    const output = new TmpFile();
    exited({
        cmd: ['hostname'],
        hostName: 'xanadu',
        pipes: [{file: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), 'xanadu\n');
});
