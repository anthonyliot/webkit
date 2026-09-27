// Helpers for the display link frame rate tests, which read uiController.displayLinkFrameRates (macOS WebKit2):
// the rate the UI-process DisplayLink asks the platform display link for, the display's nominal rate, and whether
// the platform display link runs at the requested rate (the CADisplayLink backend) or always at the nominal rate.

// The rate the display link asks for to serve a demand: the slowest nominal / k that is at least the demand
// (WebCore::displayLinkFrameRateDivisor()).
function displayLinkRateForDemand(nominalFramesPerSecond, demand)
{
    if (!demand)
        return 0;
    return nominalFramesPerSecond / Math.max(1, Math.floor(nominalFramesPerSecond / demand + 0.05));
}

// The rate at which the page renders (WebCore::preferredFramesPerSecond()).
function pageRenderingFramesPerSecond(nominalFramesPerSecond, { throttled = false, preferNear60 = true } = { })
{
    if (nominalFramesPerSecond == 60)
        return throttled ? 30 : 60;
    let framesPerSecond = nominalFramesPerSecond;
    if (preferNear60 && nominalFramesPerSecond > 60)
        framesPerSecond = Math.floor(nominalFramesPerSecond / Math.floor(nominalFramesPerSecond / 60));
    return throttled ? Math.floor(framesPerSecond / 2) : framesPerSecond;
}

// Polls the display link until isExpected(frameRates) is true or the timeout expires. frameRates is null when the page
// has no display link.
async function waitForDisplayLinkFrameRates(isExpected, timeout = 5000)
{
    const start = performance.now();
    let frameRates = null;
    while (performance.now() - start < timeout) {
        frameRates = await UIHelper.displayLinkFrameRates();
        if (isExpected(frameRates))
            return { passed: true, frameRates };
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    return { passed: false, frameRates };
}

function describeFrameRates(frameRates)
{
    if (!frameRates)
        return "no display link";
    return `requested ${frameRates.requestedFramesPerSecond} fps for observers [${frameRates.observerFramesPerSecond}], nominal ${frameRates.nominalFramesPerSecond} fps, `
        + `${frameRates.supportsPreferredFramesPerSecond ? "runs at the requested rate" : "runs at the nominal rate"}`;
}

// Waits until the display link asks for expectedFramesPerSecond(frameRates) frames per second.
async function expectRequestedFramesPerSecond(message, expectedFramesPerSecond, timeout)
{
    const result = await waitForDisplayLinkFrameRates(frameRates => frameRates && frameRates.requestedFramesPerSecond == expectedFramesPerSecond(frameRates), timeout);
    if (result.passed)
        testPassed(message);
    else
        testFailed(`${message}: ${describeFrameRates(result.frameRates)}; expected ${result.frameRates ? expectedFramesPerSecond(result.frameRates) : "?"} fps`);
    return result.frameRates;
}

// Waits until isExpected(frameRates) is true.
async function expectDisplayLinkFrameRates(message, isExpected, timeout)
{
    const result = await waitForDisplayLinkFrameRates(isExpected, timeout);
    if (result.passed)
        testPassed(message);
    else
        testFailed(`${message}: ${describeFrameRates(result.frameRates)}`);
    return result.frameRates;
}

// Reads the display link's rates right after asking for a presentation update, while its callback is pending.
function displayLinkFrameRatesWhilePresentationUpdateIsPending()
{
    const script = `
        let frameRates = null;
        uiController.doAfterPresentationUpdate(() => uiController.uiScriptComplete(JSON.stringify(frameRates)));
        frameRates = uiController.displayLinkFrameRates ?? null;`;
    return new Promise(resolve => testRunner.runUIScriptImmediately(script, frameRates => resolve(JSON.parse(frameRates))));
}

// Makes the display link behave as if the display ran at this rate, so that the page's rendering rate (about 60 fps),
// its throttled rate (30 fps), the hidden page rate (10 fps) and the display's full rate all differ, whatever the
// actual display. Reset between tests.
async function useNominalFramesPerSecond(framesPerSecond)
{
    await UIHelper.renderingUpdate();
    await UIHelper.setDisplayLinkNominalFramesPerSecond(framesPerSecond);
    await expectDisplayLinkFrameRates(`The display link uses a nominal rate of ${framesPerSecond} fps.`, frameRates => frameRates && frameRates.nominalFramesPerSecond == framesPerSecond);
}

function startAnimationFrameLoop()
{
    const box = document.getElementById("box");
    function tick(timestamp) {
        box.style.transform = `translateX(${(timestamp / 5) % 300}px)`;
        requestAnimationFrame(tick);
    }
    requestAnimationFrame(tick);
}
