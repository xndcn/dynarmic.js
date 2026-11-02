"""Exercise cache versions against real assets and the CMake page renderer."""
import hashlib
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "examples/browser/render_page.cmake"


class RenderPageTests(unittest.TestCase):
    def test_changed_assets_get_new_urls_without_changing_snippets_or_downloads(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "page.html"
            source.write_text('''<link rel="stylesheet" href="styles.css">
<script src="official-benchmarks-app.js" defer></script>
<script src="quickjs-workloads.js" defer></script>
<a href="official-benchmarks-app.js" download>Source</a>
<pre><code>npm install ./package.tgz</code></pre>''')
            css, js = root / "styles.css", root / "official-benchmarks-app.js"
            css.write_text("body { color: black; }")
            js.write_text("old UI")

            def render():
                subprocess.run(["cmake", f"-DSOURCE={source}", f"-DDIST={root}", "-P", str(SCRIPT)], check=True)
                return (root / "index.html").read_text()

            first = render()
            self.assertIn('href="styles.css?v=' + hashlib.sha256(css.read_bytes()).hexdigest() + '"', first)
            self.assertIn('src="official-benchmarks-app.js?v=' + hashlib.sha256(js.read_bytes()).hexdigest() + '"', first)
            self.assertIn('<a href="official-benchmarks-app.js" download>', first)
            self.assertIn('<pre><code>npm install ./package.tgz</code></pre>', first)
            self.assertEqual(render(), first)
            js.write_text("new UI")
            second = render()
            self.assertNotEqual(first, second)
            self.assertIn(hashlib.sha256(js.read_bytes()).hexdigest(), second)
            # Optional guest tools may be absent in a partial local build.
            self.assertIn('src="quickjs-workloads.js"', second)
            workload = root / "quickjs-workloads.js"
            workload.write_text("generated workload")
            self.assertIn('src="quickjs-workloads.js?v=' + hashlib.sha256(workload.read_bytes()).hexdigest() + '"', render())



if __name__ == "__main__":
    unittest.main()
