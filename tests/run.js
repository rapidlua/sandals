require('./generic');
require('./hostName');
require('./domainName');
require('./uid');
require('./gid');
require('./chroot');
require('./mounts');
// require('./cgroup');
// require('./seccomp');
// require('./vaRandomize');
// require('./env');
// require('./workDir');
// require('./timeLimit');
// require('./pipes');
// require('./stdStreams');

require('./security');

const { testsTotal, testsSucceeded } = require('./harness').getInfo();
console.log(`${testsSucceeded}/${testsTotal}`);
process.exit(testsTotal && testsTotal === testsSucceeded ? 0 : 1);
