import asyncio, sys
from playwright.async_api import async_playwright
async def main():
    async with async_playwright() as p:
        b = await p.chromium.launch(executable_path='/usr/bin/google-chrome', args=['--no-sandbox'])
        ctx = await b.new_context(viewport={'width': 390, 'height': 844}, device_scale_factor=2, accept_downloads=True, locale='es-ES')
        pg = await ctx.new_page()
        errs = []
        pg.on('pageerror', lambda e: errs.append(str(e)))
        pg.on('console', lambda m: m.type == 'error' and errs.append(m.text))
        await pg.goto('http://127.0.0.1:8765/')
        await pg.wait_for_timeout(2500)
        await pg.screenshot(path='/tmp/ui1_home.png')
        await pg.click('#bSetup'); await pg.fill('#ref', 'Barco Marea · ORMA 123456')
        await pg.click('#segDur button[data-v="120"]')
        await pg.screenshot(path='/tmp/ui2_setup.png')
        await pg.click('#bStart'); await pg.wait_for_timeout(3200)
        await pg.screenshot(path='/tmp/ui3_running.png')
        await pg.wait_for_selector('#term.active', timeout=15000)
        await pg.screenshot(path='/tmp/ui4_term.png')
        await pg.click('#ansTerm button[data-v="120"]')
        await pg.wait_for_selector('#result.active')
        await pg.wait_for_timeout(400)
        await pg.screenshot(path='/tmp/ui5_result.png')
        await pg.screenshot(path='/tmp/ui5_result_full.png', full_page=True)
        async with pg.expect_download() as dl:
            await pg.click('#bPdf')
        d = await dl.value
        await d.save_as('/tmp/ui_report.pdf'); print('download', d.suggested_filename)
        print('errors', errs)
        await b.close()
asyncio.run(main())
