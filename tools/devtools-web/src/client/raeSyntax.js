export async function loadRaeSyntax(url = "/rae_syntax.json") {
  const response = await fetch(url);
  if (!response.ok) throw new Error(`Failed to load Rae syntax: ${response.status}`);
  return response.json();
}
