const assert = require('assert');
const {
    test, requestInvalid, exited, internalError, TmpFile
} = require('./harness');

// Can mount tmpfs; ensure newly mounted filesystem is empty
test('mounts-tmpfs', ()=>{
    const output = new TmpFile();
    exited({
        cmd: ['ls', '/proc'],
        mounts: [{type: 'tmpfs', dest: '/proc'}],
        pipes: [{file: output, stdout: true}]
    }, 0);
    assert.equal(output.read(), '');
});

// Can mount proc; ensure ps -A enumerates processes in the new pid namespace 
test('mounts-proc', ()=>{
    const output = new TmpFile();
    exited({
        cmd: ['ps', '-A', '-o', 'pid=', '-o', 'comm='],
        mounts: [{type: 'proc', dest: '/proc'}],
        pipes: [{file: output, stdout: true}]
    }, 0);
    assert.deepEqual(
        output.read().split(/\s+/).filter(item=>item),
        ['1', 'sandals', '2', 'ps']);
});

// Can bind-mount a file into sandbox, ensure the file is present and
// contains the expected payload, can overwrite
test('mounts-bind', ()=>{
    const data = new TmpFile('0123456789abcdef');
    const output = new TmpFile();
    exited({
        cmd: ['sh', '-c', 'cat /proc/data; echo -n XXXXXX > /proc/data'],
        mounts: [
            {type: 'tmpfs', dest: '/proc'},
            {type: 'bind', dest: '/proc/data', src: data}
        ],
        pipes: [{file: output, stdout: true}]
    }, 0);
    assert.equal(data.read(), 'XXXXXX');
    assert.equal(output.read(), '0123456789abcdef');
});

// Can bind-mount a file into sandbox readonly, ensure the file is
// present and contains the expected payload, yet it's not possible to
// overwrite, even with the help of chmod +w.
test('mounts-bind-ro', ()=>{
    const data = new TmpFile('0123456789abcdef');
    const output = new TmpFile();
    exited({
        cmd: [
            'sh', '-c',
            [
                'cat /proc/data',
                'chmod +w /proc/data',
                'echo -n XXXXXX > /proc/data'
            ].join(';')
        ],
        mounts: [
            {type: 'tmpfs', dest: '/proc'},
            {type: 'bind', dest: '/proc/data', src: data, ro: true}
        ],
        pipes: [{file: output, stdout: true}]
    });
    assert.equal(data.read(), '0123456789abcdef');
    assert.equal(output.read(), '0123456789abcdef');
});

// Invalid mounts parameter
test('mounts-invalid', ()=>{

    requestInvalid({cmd:['id'], mounts:{}}, /^mounts: /);
    requestInvalid({cmd:['id'], mounts:''}, /^mounts: /);
    requestInvalid({cmd:['id'], mounts:-1}, /^mounts: /);
    requestInvalid({cmd:['id'], mounts:true}, /^mounts: /);
    requestInvalid({cmd:['id'], mounts:false}, /^mounts: /);
    requestInvalid({cmd:['id'], mounts:null}, /^mounts: /);

    internalError({cmd:['id'], mounts:[{type: 'invalid', dest: '/'}]});
    internalError({cmd:['id'], mounts:[{type: 'tmpfs', dest: '/invalid'}]});
    internalError({
        cmd:['id'],
        mounts:[{type: 'tmpfs', dest: '/tmp', options: 'invalid'}]});

    const mountsOk = [];
    for (let i = 0; i < 3; ++i) {

        const ithItemInvalid = new RegExp(`^mounts\\[${i}\\]: `);
        requestInvalid({cmd:['id'], mounts:[...mountsOk, []]}, ithItemInvalid);
        requestInvalid({cmd:['id'], mounts:[...mountsOk, {}]}, ithItemInvalid);
        requestInvalid({cmd:['id'], mounts:[...mountsOk, '']}, ithItemInvalid);
        requestInvalid({cmd:['id'], mounts:[...mountsOk, -1]}, ithItemInvalid);
        requestInvalid({cmd:['id'], mounts:[...mountsOk, true]}, ithItemInvalid);
        requestInvalid({cmd:['id'], mounts:[...mountsOk, false]}, ithItemInvalid);
        requestInvalid({cmd:['id'], mounts:[...mountsOk, null]}, ithItemInvalid);
        
        requestInvalid( // unknown key
            {cmd:['id'], mounts:[...mountsOk, {unknown: true}]},
            new RegExp(`^mounts\\[${i}\\]\.unknown: `));

        requestInvalid( // dest missing
            {cmd:['id'], mounts:[...mountsOk, {type: 'tmpfs'}]}, ithItemInvalid);
        
        requestInvalid( // type missing
            {cmd:['id'], mounts:[...mountsOk, {dest: '/'}]}, ithItemInvalid);

        requestInvalid( // src missing
            {cmd:['id'], mounts:[...mountsOk, {type: 'bind', dest: '/'}]},
            ithItemInvalid);

        // bad type
        const typeInvalid = new RegExp(`^mounts\\[${i}\\].type: `);
        requestInvalid(
            {cmd:['id'], mounts:[...mountsOk, {type: 1}]}, typeInvalid);

        // bad src
        const srcInvalid = new RegExp(`^mounts\\[${i}\\].src: `);
        requestInvalid(
            {cmd:['id'], mounts:[...mountsOk, {src: 1}]}, srcInvalid);

        // bad dest
        const destInvalid = new RegExp(`^mounts\\[${i}\\].dest: `);
        requestInvalid(
            {cmd:['id'], mounts:[...mountsOk, {dest: 1}]}, destInvalid);

        // bad options
        const optionsInvalid = new RegExp(`^mounts\\[${i}\\].options: `);
        requestInvalid(
            {cmd:['id'], mounts:[...mountsOk, {options: 1}]}, optionsInvalid);

        // bad ro
        const roInvalid = new RegExp(`^mounts\\[${i}\\].ro: `);
        requestInvalid(
            {cmd:['id'], mounts:[...mountsOk, {ro: 1}]}, roInvalid);

        mountsOk.push({type: 'bind', dest: '/', src: '/'});
    }
});
