namespace nix {

/**
 * Builder that runs builtin:fetchurl in-process (no fork) using the
 * daemon's shared FileTransfer instance.  This enables HTTP/2
 * connection pooling across concurrent FOD builds.
 */
class InProcessFetchBuilder : public DerivationBuilderImpl
{
    /**
     * Thread for in-process builtin:fetchurl execution.
     */
    std::thread fetchThread;

    /**
     * Exit status from in-process fetch (0 = success).
     */
    std::atomic<int> fetchStatus{0};

public:

    InProcessFetchBuilder(
        LocalStore & store,
        std::unique_ptr<DerivationBuilderCallbacks> miscMethods,
        DerivationBuilderParams params)
        : DerivationBuilderImpl(store, std::move(miscMethods), std::move(params))
    {
    }

    std::optional<Descriptor> startBuild() override
    {
        setupScratchOutputs();

        /* Create the log file. */
        miscMethods->openLogFile();

        buildResult.startTime = time(0);

        /* Create a pipe to signal completion to the event loop.
           The read end acts as builderOut (monitored by poll()).
           The write end is closed by the fetch thread on completion,
           producing an EOF that the event loop interprets as "build done". */
        Pipe completionPipe;
        completionPipe.create();

        /* Prepare the output path. For fixed-output derivations,
           scratchOutputs has the path we should write to.
           Use realPathInHost() to get the path that registerOutputs()
           will later check -- this handles both chroot (sandbox) and
           non-chroot (non-sandboxed) builds correctly. */
        auto outputPath = realPathInHost(store.printStorePath(scratchOutputs.at("out"))).string();

        /* Read netrc and CA data before spawning the thread. */
        std::string netrcData;
        std::string caFileData;
        try {
            netrcData = readFile(fileTransferSettings.netrcFile);
        } catch (SystemError &) {}
        if (auto & caFile = fileTransferSettings.caFile.get())
            try {
                caFileData = readFile(*caFile);
            } catch (SystemError &) {}

        auto mainUrl = drv.env.at("url");
        bool unpack = getOr(drv.env, "unpack", "") == "1";
        auto dofPtr = std::get_if<DerivationOutput::CAFixed>(&drv.outputs.at("out").raw);
        Strings hashedMirrors = settings.getLocalSettings().hashedMirrors;

        int writeFd = completionPipe.writeSide.get();
        completionPipe.writeSide.release(); // thread takes ownership

        builderOut = std::move(completionPipe.readSide);

        fetchThread = std::thread([
            this, outputPath, mainUrl, unpack, netrcData, caFileData,
            hashedMirrors, dofPtr, writeFd
        ]() {
            try {
                /* Use the daemon's shared FileTransfer instance.
                   This is the key: all concurrent builtin:fetchurl
                   derivations share the same connections. */
                auto fileTransfer = getFileTransfer();

                auto fetch = [&](const std::string & url) {
                    auto source = sinkToSource([&](Sink & sink) {
                        FileTransferRequest request(VerbatimURL{url});
                        request.decompress = false;
                        auto decompressor = makeDecompressionSink(
                            unpack && hasSuffix(mainUrl, ".xz") ? "xz" : "none", sink);
                        fileTransfer->download(std::move(request), *decompressor);
                        decompressor->finish();
                    });

                    if (unpack)
                        restorePath(outputPath, *source);
                    else
                        writeFile(outputPath, *source);
                };

                /* Try hashed mirrors first. */
                if (dofPtr && dofPtr->ca.method.getFileIngestionMethod() == FileIngestionMethod::Flat) {
                    for (auto hashedMirror : hashedMirrors) {
                        try {
                            if (!hasSuffix(hashedMirror, "/"))
                                hashedMirror += '/';
                            fetch(hashedMirror + printHashAlgo(dofPtr->ca.hash.algo) + "/"
                                + dofPtr->ca.hash.to_string(HashFormat::Base16, false));
                            goto done;
                        } catch (Error & e) {
                            debug(e.what());
                        }
                    }
                }

                fetch(mainUrl);

            done:
                fetchStatus.store(0);
            } catch (std::exception & e) {
                ignoreExceptionExceptInterrupt();
                fetchStatus.store(1);
            }
            close(writeFd);
        });

        return builderOut.get();
    }

    SingleDrvOutputs unprepareBuild() override
    {
        /* Join the fetch thread. */
        if (fetchThread.joinable())
            fetchThread.join();
        int status = fetchStatus.load();
        debug("in-process fetchurl for '%s' finished with status %d",
            store.printStorePath(drvPath), status);

        buildResult.timesBuilt++;
        buildResult.stopTime = time(0);

        /* So the fetch is done now. */
        miscMethods->childTerminated();

        /* Close the read side of the logger pipe. */
        builderOut.close();

        /* Close the log file. */
        miscMethods->closeLogFile();

        /* Check the exit status. */
        if (status != 0) {
            cleanupBuild(false);
            throw BuilderFailureError{
                BuildResult::Failure::PermanentFailure,
                1,
                "",
            };
        }

        /* Compute the FS closure of the outputs and register them as
           being valid. */
        auto builtOutputs = registerOutputs();

        cleanupBuild(true);

        return builtOutputs;
    }

    bool killChild() override
    {
        /* For in-process fetches, we can't really kill the thread,
           but we can join it and let it finish. */
        if (fetchThread.joinable())
            fetchThread.join();
        miscMethods->childTerminated();
        return true;
    }

    void killSandbox(bool) override
    {
        /* No sandbox processes to kill for in-process fetches. */
    }

    void cleanupBuild(bool force) override
    {
        if (force) {
            /* Delete unused redirected outputs (when doing hash rewriting). */
            for (auto & i : redirectedOutputs)
                deletePath(store.toRealPath(i.second));
        }
        /* No tmp dir to clean up for in-process fetches. */
    }
};

DerivationBuilderUnique makeInProcessFetchBuilder(
    LocalStore & store, std::unique_ptr<DerivationBuilderCallbacks> miscMethods, DerivationBuilderParams params)
{
    return DerivationBuilderUnique(new InProcessFetchBuilder(store, std::move(miscMethods), std::move(params)));
}

} // namespace nix
