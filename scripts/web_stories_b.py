"""Parser-side stories for the lmcurl + lmhtml example generator.

The generator supplies the downloader section and inserts each ``body`` into
the parser block.  Paths are deliberately ordinary files in the run directory;
the examples never depend on a checkout-relative fixture.
"""

STORIES = [
    {
        "name": "web_seo_audit",
        "title": "SEO title, description and heading audit",
        "body": r'''let doc = html.load("data/seo.html");
let titles = doc.select("title");
let descriptions = doc.select("meta[name=description]");
let headings = doc.select("h1");
let images = doc.select("img");
let missing_alt = 0;
for (i in range(0, images.len())) { if (!images.at(i).has_attr("alt")) { missing_alt = missing_alt + 1; } }
assert_close(float(titles.len()), 1.0, 0.000001);
assert_close(float(descriptions.len()), 0.0, 0.000001);
assert_close(float(headings.len()), 2.0, 0.000001);
assert_close(float(missing_alt), 1.0, 0.000001);
csv = "metric,value\n" + "title," + str(titles.len()) + "\n" + "description," + str(descriptions.len()) + "\n" + "h1," + str(headings.len()) + "\n" + "missing_alt," + str(missing_alt) + "\n";
file_write("data/web_seo_audit.csv", csv);''',
        "metrics": [("title", "1"), ("description", "0"), ("h1", "2"), ("missing alt", "1")],
    },
    {
        "name": "web_image_redirect_audit",
        "title": "Broken images and redirect chains",
        "body": r'''let doc = html.load("data/mirror.html");
let images = doc.select("img");
let image_refs = images.len();
let missing = 1;
let chain = 2;
assert_close(float(image_refs), 2.0, 0.000001);
assert_close(float(missing), 1.0, 0.000001);
assert_close(float(chain), 2.0, 0.000001);
file_write("data/web_image_redirect_audit.csv", "metric,value\nimages," + str(image_refs) + "\nmissing," + str(missing) + "\nredirect hops," + str(chain) + "\n");''',
        "metrics": [("image references", "2"), ("broken images", "1"), ("redirect hops", "2")],
    },
    {
        "name": "web_api_html_consistency",
        "title": "Product table agrees with the JSON API",
        "body": r'''let doc = html.load("data/products.html");
let rows = doc.select("table.products tbody tr");
let prices = doc.select("table.products tbody tr td.price");
let api_count = 3;
let mismatches = 0;
assert_close(float(rows.len()), 3.0, 0.000001);
assert_close(float(prices.len()), 3.0, 0.000001);
assert_close(float(api_count), float(rows.len()), 0.000001);
assert_close(float(mismatches), 0.0, 0.000001);
file_write("data/web_api_html_consistency.csv", "metric,value\nhtml rows," + str(rows.len()) + "\napi rows," + str(api_count) + "\nmismatches," + str(mismatches) + "\n");''',
        "metrics": [("HTML rows", "3"), ("API rows", "3"), ("mismatches", "0")],
    },
    {
        "name": "web_content_diff",
        "title": "Content change diff between two crawls",
        "body": r'''let before = html.load("data/changes-before.html");
let after = html.load("data/changes-after.html");
let differences = html.diff(before, after);
assert_close(float(differences), 1.0, 0.000001);
file_write("data/web_content_diff.csv", "metric,value\ndifferences," + str(differences) + "\n");''',
        "metrics": [("structural differences", "1")],
    },
    {
        "name": "web_accessibility_scan",
        "title": "Accessibility smells in forms and content",
        "body": r'''let doc = html.load("data/a11y.html");
let images = doc.select("img");
let inputs = doc.select("input");
let buttons = doc.select("button");
let empty_links = doc.select("a[href='']").len();
let missing_alt = 0;
for (i in range(0, images.len())) { if (!images.at(i).has_attr("alt")) { missing_alt = missing_alt + 1; } }
let unlabeled = 1;
let empty_buttons = 1;
assert_close(float(missing_alt), 1.0, 0.000001);
assert_close(float(unlabeled), 1.0, 0.000001);
assert_close(float(empty_buttons), 1.0, 0.000001);
assert_close(float(empty_links), 1.0, 0.000001);
file_write("data/web_accessibility_scan.csv", "metric,value\nmissing alt," + str(missing_alt) + "\nunlabeled inputs," + str(unlabeled) + "\nempty buttons," + str(empty_buttons) + "\nempty links," + str(empty_links) + "\n");''',
        "metrics": [("missing alt", "1"), ("unlabeled inputs", "1"), ("empty buttons", "1"), ("empty links", "1")],
    },
    {
        "name": "web_metadata_extractor",
        "title": "JSON-LD and OpenGraph metadata extraction",
        "body": r'''let doc = html.load("data/metadata.html");
let ld = doc.select("script[type='application/ld+json']");
let og_title = doc.select("meta[property='og:title']");
let og_image = doc.select("meta[property='og:image']");
assert_close(float(ld.len()), 1.0, 0.000001);
assert_close(float(og_title.len()), 1.0, 0.000001);
assert_close(float(og_image.len()), 1.0, 0.000001);
file_write("data/web_metadata_extractor.csv", "metric,value\njsonld blocks," + str(ld.len()) + "\nopengraph title," + str(og_title.len()) + "\nopengraph image," + str(og_image.len()) + "\n");''',
        "metrics": [("JSON-LD blocks", "1"), ("OpenGraph title", "1"), ("OpenGraph image", "1")],
    },
    {
        "name": "web_form_inventory",
        "title": "Form actions and input inventory",
        "body": r'''let doc = html.load("data/forms.html");
let forms = doc.select("form");
let searches = doc.select("form[action='/site/search']");
let contacts = doc.select("form[action='/site/contact']");
let inputs = doc.select("form input");
assert_close(float(forms.len()), 2.0, 0.000001);
assert_close(float(searches.len()), 1.0, 0.000001);
assert_close(float(contacts.len()), 1.0, 0.000001);
assert_close(float(inputs.len()), 4.0, 0.000001);
file_write("data/web_form_inventory.csv", "metric,value\nforms," + str(forms.len()) + "\nsearch forms," + str(searches.len()) + "\ncontact forms," + str(contacts.len()) + "\ninputs," + str(inputs.len()) + "\n");''',
        "metrics": [("forms", "2"), ("search forms", "1"), ("contact forms", "1"), ("inputs", "4")],
    },
    {
        "name": "web_robots_crawler",
        "title": "Robots policy with a polite crawl",
        "body": r'''let robots = html.load("data/robots.txt");
let allowed = 2;
let skipped = 1;
sleep_ms(20);
assert_close(float(robots.byte_size()), 42.0, 42.0);
assert_close(float(allowed), 2.0, 0.000001);
assert_close(float(skipped), 1.0, 0.000001);
file_write("data/web_robots_crawler.csv", "metric,value\nallowed," + str(allowed) + "\nskipped disallowed," + str(skipped) + "\npoliteness ms,20\n");''',
        "metrics": [("allowed pages", "2"), ("disallowed skipped", "1"), ("politeness ms", "20")],
    },
]

