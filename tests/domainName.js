const assert = require('assert');
const { test, requestInvalid, exited, TmpFile } = require('./harness');

test('domainName', ()=>{
    requestInvalid({cmd:['id'], domainName:{}}, /^domainName: /);
    requestInvalid({cmd:['id'], domainName:[]}, /^domainName: /);
    requestInvalid({cmd:['id'], domainName:0}, /^domainName: /);
    requestInvalid({cmd:['id'], domainName:true}, /^domainName: /);
    requestInvalid({cmd:['id'], domainName:false}, /^domainName: /);
    requestInvalid({cmd:['id'], domainName:null}, /^domainName: /);

    const output = new TmpFile();
    exited({
        cmd: ['domainname'],
        pipes: [{dest: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), 'sandals\n');
});

test('domainName-custom', ()=>{
    const output = new TmpFile();
    exited({
        cmd: ['domainname'],
        domainName: 'xanadu',
        pipes: [{dest: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), 'xanadu\n');
});
