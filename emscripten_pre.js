Module.noInitialRun = true;
Module.preRun = [() => {
	//a host page can hand over files, e.g. a ROM to boot, as {path: Uint8Array}
	for (const [path, data] of Object.entries(Module.blastemFiles || {})) {
		FS.mkdirTree(path.substring(0, path.lastIndexOf('/')));
		FS.writeFile(path, data);
	}
	FS.mount(IDBFS, {autoPersist: true}, '/home/web_user');
	FS.syncfs(true, (err) => {
		if (err) {
			console.log('Error loading IDBFS', err);
		}
		//and arguments, e.g. the path of that ROM
		Module.callMain(Module.blastemArgs || []);
	});
}];
