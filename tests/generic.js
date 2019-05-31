const { spawnSync } = require('child_process');
const {
    SANDALS,
    test,
    requestInvalid, exited, killed, internalError, timeLimit
} = require('./harness');

test('basic', ()=>{
    requestInvalid();
    requestInvalid({});
    requestInvalid({cmd:1}, /^cmd: /);
    requestInvalid({cmd:[]});
    requestInvalid({cmd:['id'], unknown: true}, /^unknown: /);
    internalError({cmd:['/']});
    exited({cmd:['true']}, 0);
    exited({cmd:['false']}, 1);
    exited({cmd:['sh', '-c', 'exit 42']}, 42);
    killed({cmd:['kill', '0']}, 'SIGTERM');
    killed({cmd:['kill', '-9', '0']}, 'SIGKILL');
    timeLimit({cmd:['sleep', '1'], timeLimit:0});
    timeLimit({cmd:['sleep', '1'], timeLimit:0.1});
});

test('invalid', ()=>{
    const eof = spawnSync(
        SANDALS, [], {input: '', encoding: 'utf8'}
    ).stdout;
    if (JSON.parse(eof).status !== 'requestInvalid') assert.fail(eof);

    const bad = spawnSync(
        SANDALS, [], {input: '{{{{{{{{{{{{{{{{{{{bad', encoding: 'utf8'}
    ).stdout;
    if (JSON.parse(bad).status !== 'requestInvalid') assert.fail(bad);
});
