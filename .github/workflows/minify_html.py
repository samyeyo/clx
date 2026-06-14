import re, sys

with open(sys.argv[1], 'r') as f:
    html = f.read()

# strip HTML comments
html = re.sub(r'<!--.*?-->', '', html, flags=re.DOTALL)

# collapse blank lines
html = re.sub(r'\n\s*\n', '\n', html)

with open(sys.argv[1], 'w') as f:
    f.write(html)
