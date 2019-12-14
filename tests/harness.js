Error.stackTraceLimit = Infinity;

const assert = require('assert');
const path = require('path');
const fs = require('fs');
const { spawnSync } = require('child_process');

const PROJECT_ROOT = path.dirname(__dirname);
const SANDALS = PROJECT_ROOT + '/sandals';
const STDSTREAMS_HELPER_SO = PROJECT_ROOT + '/stdstreams_helper.so';

function sandals(request) {
    return Object.setPrototypeOf(
        JSON.parse(spawnSync(
            SANDALS, [],
            { input: JSON.stringify(request), encoding: 'utf8' }
        ).stdout),
        { toString: function(){ return JSON.stringify(this); }} );
}

function requestInvalid(request, descriptionPattern) {
    const r = sandals(request);
    if (r.status !== 'requestInvalid') assert.fail(r);
    if (descriptionPattern !== undefined) {
        if (typeof(r.description) !== 'string'
            || !r.description.match(descriptionPattern)) assert.fail(r);
    }
    return r;
}

function internalError(request) {
    const r = sandals(request);
    if (r.status !== 'internalError') assert.fail(r);
    return r;
}

function exited(request, code) {
    const r = sandals(request);
    if (r.status !== 'exited') assert.fail(r);
    if (code !== undefined && r.code !== code) assert.fail(r);
    return r;
}

function killed(request, signal) {
    const r = sandals(request);
    if (r.status !== 'killed') assert.fail(r);
    if (signal !== undefined && r.signal !== signal) assert.fail(r);
    return r;
}

function timeLimit(request) {
    const r = sandals(request);
    if (r.status != 'timeLimit') assert.fail(r);
    return r;
}

function memoryLimit(request) {
    const r = sandals(request);
    if (r.status != 'memoryLimit') assert.fail(r);
    return r;
}

function pidsLimit(request) {
    const r = sandals(request);
    if (r.status != 'pidsLimit') assert.fail(r);
    return r;
}

function fileLimit(request) {
    const r = sandals(request);
    if (r.status != 'fileLimit') assert.fail(r);
    return r;
}

const cleanupHandlers = [];
process.addListener('exit', ()=>{
    while (cleanupHandlers.length !== 0) cleanupHandlers.pop().call();
});

let testsTotal = 0;
let testsSucceeded = 0;
function test(name, fn) {
    const lengthBeforeTest = cleanupHandlers.length;
    try {
        testsTotal += 1;
        fn();
        testsSucceeded += 1;
    } catch(e) {
        // discard frames in the stack trace starting with the current one
        const n = new Error().stack.replace(/[^\n]/g,'').length;
        console.log(name + ':', e.stack.split('\n').slice(0, -n).join('\n'));
    }
    while (cleanupHandlers.length > lengthBeforeTest)
        cleanupHandlers.pop().call();
}

function getInfo() {
    return { testsTotal, testsSucceeded };
}

function randomName() {
    let name;
    do name = Math.random().toString(36).substr(2, 8); while (name.length < 8);
    return name;
}

function TmpFile(data) {
    for (;;) {
        const path = '/tmp/' + randomName();
        try {
            const fd = fs.openSync(
                path, fs.constants.O_RDWR
                | fs.constants.O_CREAT | fs.constants.O_EXCL);
            cleanupHandlers.push(()=>{
                fs.closeSync(fd);
                fs.unlinkSync(path);
            });
            if (data) fs.writeSync(fd, data, 0, data.length, 0);
            this.fd = fd;
            this.toJSON = ()=>path;
            this.readFileSync = encoding=>fs.readFileSync(fd, encoding);
            this.read = ()=>fs.readFileSync(fd, 'utf8');
            return;
        } catch (e) {
            if (e.code !== 'EEXIST') throw e;
        }
    }
}

module.exports = {
    PROJECT_ROOT,
    SANDALS,
    STDSTREAMS_HELPER_SO,
    requestInvalid,
    internalError,
    exited,
    killed,
    timeLimit,
    memoryLimit,
    pidsLimit,
    fileLimit,
    test,
    testAtExit: fn=>cleanupHandlers.push(fn),
    getInfo,
    TmpFile
};
